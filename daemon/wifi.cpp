#include "wifi.h"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "aow/log.h"
#include "shared.h"

extern char** environ;

namespace aow {

namespace {

constexpr const char* kConf = "/etc/aow-hat/wifi.conf";  // fallback=1|0
constexpr const char* kRegdom = "/sys/module/cfg80211/parameters/ieee80211_regdom";
constexpr int kRefreshS = 3;
constexpr int kScanEvery = 5;          // refreshes
constexpr int kFallbackAfterS = 90;    // after boot, with no known network found
constexpr int kBootWindowS = 300;      // a daemon started later than this is not after a boot

// Runs a program without a shell; its output (stdout and stderr) in out. Its exit status, or -1
// if it could not be run or took longer than timeoutS (it is then killed).
int spawn(const wifi::Args& args, std::string* out = nullptr, int timeoutS = 10) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    // nmcli's messages in English, whatever the unit's locale: they are shown on the page.
    std::vector<char*> env;
    for (char** e = environ; *e; ++e)
        if (std::strncmp(*e, "LC_ALL=", 7) && std::strncmp(*e, "LANG=", 5)) env.push_back(*e);
    static char lcAll[] = "LC_ALL=C";
    env.push_back(lcAll);
    env.push_back(nullptr);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), env.data());
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        return -1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutS);
    bool timedOut = false;
    char buf[1024];
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) { timedOut = true; break; }
        pollfd p{fds[0], POLLIN, 0};
        if (poll(&p, 1, static_cast<int>(left)) <= 0) continue;
        const ssize_t n = read(fds[0], buf, sizeof buf);
        if (n <= 0) break;
        if (out) out->append(buf, static_cast<size_t>(n));
    }
    close(fds[0]);
    if (timedOut) kill(pid, SIGKILL);
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || timedOut) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// nmcli's message, one line, for the page: "Error: " and trailing whitespace dropped.
std::string message(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    const size_t nl = s.rfind('\n');
    if (nl != std::string::npos) s = s.substr(nl + 1);
    if (s.rfind("Error: ", 0) == 0) s = s.substr(7);
    return s.empty() ? "no reason given" : s;
}

double uptimeS() {
    std::ifstream in("/proc/uptime");
    double s = 0;
    in >> s;
    return s;
}

bool readFallback() {
    std::ifstream in(kConf);
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("fallback=", 0) == 0) return line.substr(9) != "0";
    return true;  // on unless switched off
}

std::string readCountry() {
    std::ifstream in(kRegdom);
    std::string c;
    in >> c;
    return c;
}

}  // namespace

WifiControl::WifiControl() {
    thread_ = std::thread([this] { run(); });
}

WifiControl::~WifiControl() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

wifi::State WifiControl::state() {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::vector<wifi::Seen> WifiControl::seen() {
    std::lock_guard<std::mutex> lock(mutex_);
    return seen_;
}

bool WifiControl::apply(const wifi::Request& r, bool activate, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state_.available) {
        error = state_.unavailable.empty() ? "Wi-Fi is not available" : state_.unavailable;
        return false;
    }
    if (pending_ || state_.job == "switching") {
        error = "A Wi-Fi change is already under way";
        return false;
    }
    bool known = false;
    if (r.mode == wifi::Mode::AccessPoint) known = state_.apExists;
    error = wifi::requestError(r, known, state_.country);
    if (!error.empty()) return false;
    if (r.mode == wifi::Mode::Client && !activate) {
        error = "Nothing to do";
        return false;
    }
    pending_ = Job{r, activate};
    state_.job = "switching";
    state_.message = activate ? "Switching shortly" : "Saving";
    wake_.notify_all();
    return true;
}

bool WifiControl::setFallback(bool on, std::string& error) {
    if (!writeFileAtomically(kConf, std::string("fallback=") + (on ? "1" : "0") + "\n", error)) return false;
    LOG_NOTICE(Network, "Wi-Fi: access point fallback after boot %s", on ? "on" : "off");
    std::lock_guard<std::mutex> lock(mutex_);
    state_.fallback = on;
    return true;
}

void WifiControl::run() {
    pthread_setname_np(pthread_self(), "aow-wifi");
    const bool afterBoot = uptimeS() < kBootWindowS;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fallbackDone_ = !afterBoot;
    }
    int refreshes = 0;
    for (;;) {
        std::optional<Job> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock, std::chrono::seconds(kRefreshS), [this] { return stopping_ || pending_.has_value(); });
            if (stopping_) return;
            if (pending_) {
                // The page's reply goes out before the connection it came on may go.
                wake_.wait_for(lock, std::chrono::seconds(2), [this] { return stopping_; });
                if (stopping_) return;
                job = std::move(pending_);
                pending_.reset();
            }
        }
        if (job) carryOut(*job);
        const bool scanNow = refreshes++ % kScanEvery == 0 || job;
        refresh();
        if (scanNow) {
            bool ap = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ap = state_.apActive || !state_.available;
            }
            if (!ap) {
                std::string out;
                if (spawn({"nmcli", "-t", "-f", "SSID,SIGNAL,SECURITY", "device", "wifi", "list", "ifname", wifi::kDevice,
                         "--rescan", "no"}, &out) == 0) {
                    std::vector<wifi::Seen> s = wifi::parseScan(out);
                    std::lock_guard<std::mutex> lock(mutex_);
                    seen_ = std::move(s);
                }
            }
        }
        watchFallback();
    }
}

std::vector<WifiControl::Profile> WifiControl::profiles() {
    std::vector<Profile> out;
    std::string list;
    if (spawn({"nmcli", "-t", "-f", "UUID,TYPE,AUTOCONNECT,NAME", "connection", "show"}, &list) != 0) return out;
    size_t at = 0;
    while (at < list.size()) {
        size_t end = list.find('\n', at);
        if (end == std::string::npos) end = list.size();
        const std::vector<std::string> f = wifi::splitTerse(list.substr(at, end - at));
        at = end + 1;
        if (f.size() < 4 || f[1] != "802-11-wireless") continue;
        Profile p;
        p.uuid = f[0];
        p.autoconnect = f[2] == "yes";
        p.name = f[3];
        std::string detail;
        if (spawn({"nmcli", "-t", "-f", "802-11-wireless.ssid,802-11-wireless.mode,802-11-wireless.band,802-11-wireless.channel",
                 "connection", "show", "uuid", p.uuid}, &detail) != 0)
            continue;
        p.ssid = wifi::field(detail, "802-11-wireless.ssid");
        p.mode = wifi::field(detail, "802-11-wireless.mode");
        p.band = wifi::field(detail, "802-11-wireless.band");
        p.channel = std::atoi(wifi::field(detail, "802-11-wireless.channel").c_str());
        out.push_back(p);
    }
    return out;
}

void WifiControl::refresh() {
    wifi::State s;
    char host[64] = {};
    gethostname(host, sizeof host - 1);
    s.hostname = host;
    s.country = readCountry();
    s.fallback = readFallback();
    std::string devices;
    if (access("/usr/bin/nmcli", X_OK) != 0) {
        s.unavailable = "NetworkManager (nmcli) is not installed on this unit";
    } else if (spawn({"nmcli", "-t", "-f", "DEVICE,TYPE,STATE", "device"}, &devices) != 0) {
        s.unavailable = "NetworkManager is not running";
    } else if (devices.find(std::string(wifi::kDevice) + ":wifi:") == std::string::npos) {
        s.unavailable = "This unit has no Wi-Fi (wlan0)";
    } else {
        s.available = true;
    }
    if (s.available) {
        std::string active, dev;
        std::string activeUuid;
        if (spawn({"nmcli", "-t", "-f", "UUID,DEVICE", "connection", "show", "--active"}, &active) == 0) {
            size_t at = 0;
            while (at < active.size()) {
                size_t end = active.find('\n', at);
                if (end == std::string::npos) end = active.size();
                const std::vector<std::string> f = wifi::splitTerse(active.substr(at, end - at));
                at = end + 1;
                if (f.size() >= 2 && f[1] == wifi::kDevice) activeUuid = f[0];
            }
        }
        if (spawn({"nmcli", "-t", "-f", "GENERAL.STATE,IP4.ADDRESS", "device", "show", wifi::kDevice}, &dev) == 0) {
            s.connected = wifi::field(dev, "GENERAL.STATE").rfind("100", 0) == 0;
            s.ip = wifi::field(dev, "IP4.ADDRESS[1]");
            const size_t slash = s.ip.find('/');
            if (slash != std::string::npos) s.ip.resize(slash);
        }
        for (const Profile& p : profiles()) {
            const bool isAp = p.name == wifi::kApProfile;
            if (isAp) {
                s.apExists = true;
                s.apSsid = p.ssid;
                s.apBand5 = p.band == "a";
                s.apChannel = static_cast<uint8_t>(p.channel);
                s.mode = p.autoconnect ? wifi::Mode::AccessPoint : wifi::Mode::Client;
            } else if (p.mode != "ap" && p.mode != "adhoc") {
                s.known.push_back(p.ssid);
            }
            if (p.uuid == activeUuid) {
                s.ssid = p.ssid;
                s.apActive = isAp;
            }
        }
        if (!s.connected) s.ssid.clear();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    s.job = state_.job;
    s.message = state_.message;
    s.fallbackActive = state_.fallbackActive && s.apActive && s.mode == wifi::Mode::Client;
    if (pending_) s.job = "switching";
    state_ = std::move(s);
}

void WifiControl::finish(const char* job, const std::string& msg) {
    if (!std::strcmp(job, "failed")) LOG_WARNING(Network, "Wi-Fi: %s", msg.c_str());
    else LOG_NOTICE(Network, "Wi-Fi: %s", msg.c_str());
    std::lock_guard<std::mutex> lock(mutex_);
    state_.job = job;
    state_.message = msg;
}

void WifiControl::carryOut(const Job& job) {
    const wifi::Request& r = job.request;
    std::vector<Profile> all = profiles();
    const Profile* ap = nullptr;
    for (const Profile& p : all)
        if (p.name == wifi::kApProfile) ap = &p;
    std::string previous, previousSsid;
    bool previousAp = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        previousSsid = state_.connected ? state_.ssid : "";
        previousAp = state_.apActive;
    }
    std::string active;
    if (spawn({"nmcli", "-t", "-f", "UUID,DEVICE", "connection", "show", "--active"}, &active) == 0) {
        for (const Profile& p : all)
            if (active.find(p.uuid + ":" + wifi::kDevice) != std::string::npos) previous = p.uuid;
    }
    std::string out;
    // Back to what the unit had; "" if it had nothing.
    auto goBack = [&]() -> std::string {
        if (previous.empty()) return "";
        if (spawn(wifi::upArgs(previous, 45), nullptr, 60) == 0) {
            return previousAp ? "back on the access point" : "back on \"" + previousSsid + "\"";
        }
        return "and the connection before it did not come back either";
    };

    if (r.mode == wifi::Mode::AccessPoint) {
        if (spawn(wifi::apProfileArgs(r, ap != nullptr), &out) != 0) {
            finish("failed", "The access point could not be saved: " + message(out));
            return;
        }
        if (!job.activate) {
            finish("done", "Access point \"" + r.ssid + "\" saved; it comes up only in access point mode or as the fallback");
            return;
        }
        std::string apUuid;
        for (const Profile& p : profiles())
            if (p.name == wifi::kApProfile) apUuid = p.uuid;
        out.clear();
        if (!apUuid.empty() && spawn(wifi::upArgs(apUuid, 30), &out, 45) == 0) {
            spawn(wifi::apAutoconnectArgs(true));
            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_.fallbackActive = false;
            }
            finish("done", "Access point \"" + r.ssid + "\" is on. Tablets join it and open http://" +
                               std::string(wifi::kApAddress) + ":5174 for AgOpenWeb");
            return;
        }
        spawn(wifi::apAutoconnectArgs(false));
        const std::string back = goBack();
        finish("failed", "The access point did not start (" + message(out) + ")" + (back.empty() ? "" : "; " + back));
        return;
    }

    // Client: the network the unit knows by that SSID, or a new one.
    std::string uuid;
    for (const Profile& p : all)
        if (p.name != wifi::kApProfile && p.mode != "ap" && p.ssid == r.ssid) uuid = p.uuid;
    const bool created = uuid.empty();
    if (spawn(wifi::clientProfileArgs(r, uuid), &out) != 0) {
        finish("failed", "\"" + r.ssid + "\" could not be saved: " + message(out));
        return;
    }
    if (created)
        for (const Profile& p : profiles())
            if (p.name != wifi::kApProfile && p.mode != "ap" && p.ssid == r.ssid) uuid = p.uuid;
    if (uuid.empty()) {
        finish("failed", "\"" + r.ssid + "\" was saved but cannot be found again");
        return;
    }
    if (previousAp) {
        // An access point does not scan: take it down and look around first.
        spawn({"nmcli", "connection", "down", "uuid", previous}, nullptr, 15);
        spawn({"nmcli", "device", "wifi", "rescan", "ifname", wifi::kDevice});
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    out.clear();
    if (spawn(wifi::upArgs(uuid, 45), &out, 60) == 0) {
        if (ap) spawn(wifi::apAutoconnectArgs(false));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.fallbackActive = false;
        }
        finish("done", "Joined \"" + r.ssid + "\"");
        return;
    }
    if (created) spawn({"nmcli", "connection", "delete", "uuid", uuid});
    const std::string back = goBack();
    finish("failed", "Could not join \"" + r.ssid + "\" (" + message(out) + ")" + (back.empty() ? "" : "; " + back));
}

// In client mode, after boot: no known network found in kFallbackAfterS, the access point comes
// up (its autoconnect stays off, so the next boot tries the known networks again).
void WifiControl::watchFallback() {
    wifi::State s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fallbackDone_) return;
        if (uptimeS() < kFallbackAfterS) return;
        fallbackDone_ = true;
        s = state_;
    }
    if (!s.available || s.mode != wifi::Mode::Client || !s.fallback || s.connected || s.job == "switching") return;
    if (!s.apExists) {
        LOG_WARNING(Network, "Wi-Fi: no known network found after boot, and no access point set up to fall back to");
        return;
    }
    std::string out;
    if (spawn({"nmcli", "--wait", "30", "connection", "up", wifi::kApProfile}, &out, 45) == 0) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.fallbackActive = true;
        }
        finish("done", "No known network found after boot: access point \"" + s.apSsid + "\" is on as the fallback");
        refresh();
    } else {
        finish("failed", "No known network found after boot, and the fallback access point did not start (" + message(out) + ")");
    }
}

}  // namespace aow
