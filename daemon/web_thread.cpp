#include "web_thread.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <map>

#include "api.h"
#include "aow/gvret.h"
#include "aow/helper_status.h"
#include "aow/pgn.h"
#include "aow/log.h"
#include "linux_io.h"
#include "server.h"

namespace aow {

namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kUpdateDir = "/var/lib/aow-hat/update";  // shared with deploy/aow-hat-update
constexpr const char* kLinkdUnit = "/etc/systemd/system/aow-linkd.service";  // install.sh puts it there
constexpr const char* kLinkdWanted = "/etc/systemd/system/multi-user.target.wants/aow-linkd.service";
constexpr const char* kLinkdConf = "/etc/aow-hat/linkd.conf";  // read by aow-linkd (its --config)
// The gateway's DNS-SD announcement (_aowlink._tcp), a static avahi service: there while the
// gateway is on and listens on the network, so tablets find it by name. Avahi notices the file.
constexpr const char* kAvahiDir = "/etc/avahi/services";
constexpr const char* kAvahiService = "/etc/avahi/services/aow-linkd.service";

// Console: stderr, which systemd puts in the journal. Under systemd (JOURNAL_STREAM set) each
// line starts with <N> so the journal records its priority; in a terminal, the firmware's format.
void consoleOut(const log::Entry& e) {
    static const bool journal = std::getenv("JOURNAL_STREAM") != nullptr;
    if (journal) {
        std::fprintf(stderr, "<%d>%s/%s: %s\n", static_cast<int>(e.severity), log::severityName(e.severity),
                     log::sourceName(e.source), e.message);
    } else {
        const uint32_t t = e.timestampMs;
        std::fprintf(stderr, "[%02u:%02u:%02u.%03u] %s/%s: %s\n", t / 3600000 % 24, t / 60000 % 60, t / 1000 % 60,
                     t % 1000, log::severityName(e.severity), log::sourceName(e.source), e.message);
    }
}

// UDP syslog, RFC 3164, broadcast as the firmware does. Facility local0 + source.
class SyslogOut {
public:
    SyslogOut() {
        fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        const int one = 1;
        if (fd_ >= 0) setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
        gethostname(host_, sizeof host_ - 1);
    }
    ~SyslogOut() { if (fd_ >= 0) close(fd_); }

    void send(const log::Entry& e, uint16_t port) {
        if (fd_ < 0) return;
        const int pri = (16 + static_cast<int>(e.source)) * 8 + static_cast<int>(e.severity);
        char stamp[32];
        const time_t now = time(nullptr);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(stamp, sizeof stamp, "%b %e %H:%M:%S", &tm);
        char msg[512];
        const int n = std::snprintf(msg, sizeof msg, "<%d>%s %s aow-hatd-%s[%u]: %s", pri, stamp, host_,
                                    log::sourceName(e.source), e.sequence, e.message);
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        sendto(fd_, msg, static_cast<size_t>(n > 0 ? n : 0), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
    }

private:
    int fd_ = -1;
    char host_[64] = {};
};

uint32_t nowMicros() {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()).count());
}

// GVRET on TCP: one SavvyCAN client at a time, as the firmware.
class GvretServer {
public:
    GvretServer(Shared& shared, uint16_t port) : shared_(shared) {
        if (!port) return;
        fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        const int one = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        if (fd_ < 0 || bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0 || listen(fd_, 1) < 0) {
            LOG_ERROR(Can, "GVRET: cannot listen on port %u: %s", port, std::strerror(errno));
            if (fd_ >= 0) close(fd_);
            fd_ = -1;
            return;
        }
        LOG_INFO(Can, "GVRET (SavvyCAN) on port %u", port);
    }
    ~GvretServer() {
        dropClient();
        if (fd_ >= 0) close(fd_);
    }

    void poll() {
        if (fd_ < 0) return;
        if (client_ < 0) {
            client_ = accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (client_ < 0) return;
            proto_.reset();
            LOG_INFO(Can, "GVRET client connected");
        }
        uint8_t buf[512];
        ssize_t n;
        while ((n = recv(client_, buf, sizeof buf, 0)) > 0) out_ += proto_.input(buf, static_cast<size_t>(n), nowMicros());
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            dropClient();
            return;
        }
        shared_.gvretListening = proto_.binaryMode();
        MirrorFrame frames[128];
        size_t got;
        while ((got = shared_.takeMirror(frames, 128)) > 0)
            for (size_t i = 0; i < got; ++i)
                gvret::encodeFrame(out_, frames[i].bus, frames[i].id, frames[i].extended, frames[i].data,
                                   frames[i].len, frames[i].micros);
        if (!out_.empty()) {
            const ssize_t w = send(client_, out_.data(), out_.size(), MSG_NOSIGNAL);
            if (w > 0) out_.erase(0, static_cast<size_t>(w));
            else if (errno != EAGAIN && errno != EWOULDBLOCK) dropClient();
            if (out_.size() > (1u << 20)) out_.clear();  // a stalled client loses frames, not memory
        }
    }

private:
    void dropClient() {
        if (client_ < 0) return;
        close(client_);
        client_ = -1;
        out_.clear();
        shared_.gvretListening = false;
        LOG_INFO(Can, "GVRET client disconnected");
    }
    Shared& shared_;
    int fd_ = -1, client_ = -1;
    gvret::Protocol proto_;
    std::string out_;
};

// What the helper services report on 127.0.0.1:8891 (aow/helper_status.h). Shown, never acted on.
// The web thread's own: polled in its loop and read by the API, which runs on it too.
class HelperListener {
public:
    HelperListener() {
        fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(helper::kPort);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (fd_ < 0 || bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) {
            LOG_WARNING(System, "Helper status: cannot listen on 127.0.0.1:%u: %s", helper::kPort, std::strerror(errno));
            if (fd_ >= 0) close(fd_);
            fd_ = -1;
        }
    }
    ~HelperListener() { if (fd_ >= 0) close(fd_); }

    void poll() {
        if (fd_ < 0) return;
        uint8_t buf[512];
        ssize_t n;
        while ((n = recv(fd_, buf, sizeof buf, 0)) > 0) {
            pgn::Frame f;
            if (!pgn::parse(buf, static_cast<size_t>(n), f)) continue;
            if (f.src == helper::kSrcLinkGateway && f.pgn == helper::kPgnLinkGateway) {
                helper::LinkGateway g;
                if (!helper::decode(f.data, f.len, g)) continue;
                if (!heard_ || g.clientConnected != link_.clientConnected) {
                    if (g.clientConnected) LOG_INFO(Network, "AOW link gateway: terminal \"%s\" connected from %s", g.client, g.clientAddress);
                    else LOG_INFO(Network, "AOW link gateway: running, no terminal connected");
                }
                link_ = g;
                linkAt_ = Clock::now();
                heard_ = true;
            }
        }
        if (heard_ && Clock::now() - linkAt_ > std::chrono::milliseconds(helper::kStaleMs)) {
            heard_ = false;
            LOG_INFO(Network, "AOW link gateway: not running (no report for 3 s)");
        }
    }

    bool linkGateway(helper::LinkGateway& g) const {
        if (heard_) g = link_;
        return heard_;
    }

private:
    int fd_ = -1;
    bool heard_ = false;
    helper::LinkGateway link_;
    Clock::time_point linkAt_;
};

class DaemonBackend : public web::Backend {
public:
    DaemonBackend(Shared& shared, const SettingsFile& file, HelperListener& helpers)
        : shared_(shared), file_(file), helpers_(helpers) {
        syncAnnouncement(linkGatewayEnabled());  // after an update or a hand edit
    }

    Settings settings() override { return shared_.settings(); }

    bool applySettings(const Settings& s, std::string& error) override {
        if (!file_.save(s, error)) {
            LOG_ERROR(Config, "Settings not saved: %s", error.c_str());
            return false;
        }
        shared_.setSettings(s);
        log::global().setConfig(s.log);
        LOG_INFO(Config, "Settings saved to %s", file_.path.c_str());
        return true;
    }

    Status status() override { return shared_.status(); }
    IsobusStatus isobusStatus() override { return shared_.isobus(); }
    bool linkGateway(helper::LinkGateway& g) override { return helpers_.linkGateway(g); }

    // The gateway's service (deploy/aow-linkd.service): installed by install.sh, switched on and
    // off here as the installer's --gateway and --no-gateway do. Enabled is read from systemd's
    // link, not by running systemctl: the ISOBUS page asks every second.
    bool linkGatewayInstalled() override { return access(kLinkdUnit, F_OK) == 0; }
    bool linkGatewayEnabled() override { return access(kLinkdWanted, F_OK) == 0; }
    bool setLinkGatewayEnabled(bool on, std::string& error) override {
        LOG_NOTICE(Network, "AOW link gateway switched %s from the ISOBUS page", on ? "on" : "off");
        // --no-block: the start or stop is queued, not waited for; the page sees the result in
        // the gateway's reports.
        const char* cmd = on ? "systemctl --no-block enable --now aow-linkd.service >/dev/null 2>&1"
                             : "systemctl --no-block disable --now aow-linkd.service >/dev/null 2>&1";
        if (std::system(cmd) != 0) {
            error = on ? "The gateway could not be switched on (systemctl failed)"
                       : "The gateway could not be switched off (systemctl failed)";
            LOG_ERROR(Network, "%s", error.c_str());
            return false;
        }
        syncAnnouncement(on);
        return true;
    }

    LinkGatewaySettings linkGatewaySettings() override {
        LinkGatewaySettings s;
        std::ifstream in(kLinkdConf);
        std::string line;
        while (std::getline(in, line)) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            if (line.compare(0, eq, "name") == 0) s.name = line.substr(eq + 1);
            else if (line.compare(0, eq, "network") == 0) s.network = line.substr(eq + 1) == "1";
        }
        return s;
    }

    std::string linkGatewayDefaultName() override {
        char host[64] = {};
        gethostname(host, sizeof host - 1);
        return std::string("AOW HAT ") + host;
    }

    bool linkGatewayAnnounced() override { return access(kAvahiService, F_OK) == 0; }

    bool setLinkGatewaySettings(const LinkGatewaySettings& in, std::string& error) override {
        // One DNS label (at most 63 bytes), printable, no line breaks: it goes into a key=value
        // file, an XML file and the gateway's HELLO.
        std::string name;
        for (char c : in.name)
            if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F) name += c;
        while (!name.empty() && name.back() == ' ') name.pop_back();
        while (!name.empty() && name.front() == ' ') name.erase(0, 1);
        if (name.size() > 63) {
            error = "The name is too long (at most 63 characters)";
            return false;
        }
        if (!writeFileAtomically(kLinkdConf, "name=" + name + "\nnetwork=" + (in.network ? "1" : "0") + "\n", error))
            return false;
        LOG_NOTICE(Network, "AOW link gateway: name \"%s\", %s", name.empty() ? linkGatewayDefaultName().c_str() : name.c_str(),
                   in.network ? "tablets on the network may connect" : "this unit only");
        syncAnnouncement(linkGatewayEnabled());
        // Running: restarted to take them (a connected terminal is back within about a second).
        if (std::system("systemctl --no-block try-restart aow-linkd.service >/dev/null 2>&1") != 0)
            LOG_WARNING(Network, "AOW link gateway not restarted (systemctl failed); its new settings apply at its next start");
        return true;
    }

    void restart() override {
        LOG_WARNING(System, "Restart requested from the web page");
        // systemd's Restart=always brings the daemon back.
        kill(getpid(), SIGTERM);
    }

    std::string customCanConfig() override {
        std::ifstream in(canPath());
        std::stringstream s;
        s << in.rdbuf();
        return s.str();
    }
    bool writeCustomCanConfig(const std::string& json, std::string& error) override {
        return writeFileAtomically(canPath(), json, error);
    }
    bool deleteCustomCanConfig(std::string& error) override {
        if (unlink(canPath().c_str()) == 0 || errno == ENOENT) return true;
        error = std::strerror(errno);
        return false;
    }

    bool um98xRead(Um98x::Config& c, std::string& error) override {
        return withReceiver(error, [&](Um98x& um) { return um.readConfiguration(c); });
    }
    bool um98xWrite(const Um98x::Config& c, std::string& error) override {
        return withReceiver(error, [&](Um98x& um) { return um.writeConfiguration(c); });
    }

    bool startUpdate(const std::string& package, std::string& error) override {
        if (mkdir(kUpdateDir, 0700) != 0 && errno != EEXIST) {
            error = std::string(kUpdateDir) + ": " + std::strerror(errno);
            return false;
        }
        const std::string dir = kUpdateDir;
        if (!writeFileAtomically(dir + "/incoming.aowpkg", package, error)) return false;
        std::string ignored;
        writeFileAtomically(dir + "/status.json", R"({"state":"received","message":"Package received."})", ignored);
        // The updater is its own unit: it restarts this daemon, so it cannot be this daemon.
        if (std::system("systemctl start --no-block aow-hat-update.service") != 0) {
            error = "The updater service could not be started (is this unit installed with install.sh?)";
            writeFileAtomically(dir + "/status.json", R"({"state":"failed","message":"The updater service could not be started."})", ignored);
            return false;
        }
        return true;
    }
    std::string updateStatus() override {
        std::ifstream in(std::string(kUpdateDir) + "/status.json");
        std::stringstream s;
        s << in.rdbuf();
        return s.str();
    }
    void reboot() override {
        LOG_WARNING(System, "Unit restart requested from the System Update page");
        if (std::system("systemctl reboot") != 0) LOG_ERROR(System, "systemctl reboot failed");
    }

    std::vector<Interface> interfaces() override {
        std::vector<Interface> out;
        ifaddrs* list = nullptr;
        if (getifaddrs(&list) != 0) return out;
        for (ifaddrs* i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            if (i->ifa_flags & IFF_LOOPBACK) continue;
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, ip, sizeof ip);
            Interface f{i->ifa_name, ip, (i->ifa_flags & IFF_RUNNING) != 0, 0};
            std::ifstream speed(std::string("/sys/class/net/") + i->ifa_name + "/speed");
            if (!(speed >> f.speedMbps) || f.speedMbps < 0) f.speedMbps = 0;
            out.push_back(f);
        }
        freeifaddrs(list);
        return out;
    }

private:
    static std::string xmlEscape(const std::string& s) {
        std::string out;
        for (char c : s) {
            switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
            }
        }
        return out;
    }

    // The announcement follows the gateway: there while it is on and listens on the network.
    // Without avahi on the unit there is nothing to announce through, and nothing is written.
    void syncAnnouncement(bool enabled) {
        if (!linkGatewayInstalled() || access(kAvahiDir, F_OK) != 0) return;
        const LinkGatewaySettings s = linkGatewaySettings();
        if (!enabled || !s.network) {
            if (unlink(kAvahiService) == 0) LOG_INFO(Network, "AOW link gateway no longer announced on the network");
            return;
        }
        const std::string name = s.name.empty() ? linkGatewayDefaultName() : s.name;
        const std::string xml =
            "<?xml version=\"1.0\" standalone='no'?>\n"
            "<!DOCTYPE service-group SYSTEM \"avahi-service.dtd\">\n"
            "<!-- Written by aow-hatd from the ISOBUS page: the AOW link gateway (aow-linkd). -->\n"
            "<service-group>\n"
            "  <name>" + xmlEscape(name) + "</name>\n"
            "  <service>\n"
            "    <type>_aowlink._tcp</type>\n"
            "    <port>2323</port>\n"
            "    <txt-record>v=1</txt-record>\n"
            "  </service>\n"
            "</service-group>\n";
        std::string error;
        if (writeFileAtomically(kAvahiService, xml, error))
            LOG_INFO(Network, "AOW link gateway announced on the network as \"%s\"", name.c_str());
        else
            LOG_WARNING(Network, "AOW link gateway not announced: %s", error.c_str());
    }

    // Borrows the GNSS port from the control thread for one UM98x operation.
    template <typename Fn>
    bool withReceiver(std::string& error, Fn fn) {
        hal::SerialPort* port = shared_.gnssPort.load();
        if (!port) {
            error = "No GNSS receiver configured (aow-hatd --gnss)";
            return false;
        }
        shared_.gnssPauseRequested = true;
        for (int i = 0; i < 100 && !shared_.gnssPaused; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!shared_.gnssPaused) {
            shared_.gnssPauseRequested = false;
            error = "The control loop did not release the GNSS port";
            return false;
        }
        LOG_INFO(Gnss, "UM98x: GNSS stream paused for receiver setup");
        linux_io::MonotonicClock clock;
        Um98x um(*port, clock, [](uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); });
        const bool ok = fn(um);
        shared_.gnssPauseRequested = false;
        LOG_INFO(Gnss, "UM98x: %s; GNSS stream resumed", ok ? "done" : "failed");
        return ok;
    }

    std::string canPath() const { return file_.canConfigPath(); }
    Shared& shared_;
    const SettingsFile& file_;
    HelperListener& helpers_;
};

}  // namespace

void WebThread::run() {
    // systemd starts the whole process SCHED_FIFO; the web thread must not compete with the
    // control loop.
    sched_param normal{};
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &normal);
    pthread_setname_np(pthread_self(), "aow-web");

    HelperListener helpers;
    DaemonBackend backend(shared_, opt_.settingsFile, helpers);
    web::Server server;
    server.maxBody = web::kMaxUpdatePackage;  // an update package is the largest upload
    bool pages = false;
    if (opt_.httpPort) {
        pages = server.listenHttp(opt_.httpPort) != 0;
        pages = server.listenWebSocket(web::kTelemetryPort) != 0 && pages;
        pages = server.listenWebSocket(web::kLogPort) != 0 && pages;
        server.greet(web::kLogPort, web::logHistoryJson);
        web::addRoutes(server, backend, opt_.version);
        if (pages) LOG_INFO(Network, "Web pages on port %u", opt_.httpPort);
    }

    GvretServer gvretServer(shared_, opt_.gvretPort);
    SyslogOut syslog;
    log::Logger::Outputs outputs;
    outputs.console = consoleOut;
    outputs.udp = [&](const log::Entry& e) { syslog.send(e, log::global().config().syslogPort); };
    outputs.web = [&](const log::Entry& e) {
        if (!server.clientCount(web::kLogPort)) return;
        const std::string j = web::logEntryJson(e);
        server.broadcast(web::kLogPort, j.data(), j.size(), false);
    };

    constexpr auto kTelemetryPeriod = std::chrono::milliseconds(10);  // 100 Hz, as the firmware
    auto nextTelemetry = Clock::now();
    auto nextNetworkCheck = Clock::now();
    while (!shared_.stop.load()) {
        server.poll(5);
        gvretServer.poll();
        helpers.poll();
        if (Clock::now() >= nextNetworkCheck) {
            nextNetworkCheck = Clock::now() + std::chrono::seconds(1);
            bool up = false;
            size_t n = 0;
            for (const auto& i : backend.interfaces()) {
                up = up || (i.up && !i.ip.empty());
                in_addr a{};
                if (n < Shared::kOwnAddresses && inet_pton(AF_INET, i.ip.c_str(), &a) == 1)
                    shared_.ownAddresses[n++].store(a.s_addr, std::memory_order_relaxed);
            }
            while (n < Shared::kOwnAddresses) shared_.ownAddresses[n++].store(0, std::memory_order_relaxed);
            shared_.networkUp = up;
        }
        if (shared_.takeSaveRequest()) {
            std::string err;
            if (opt_.settingsFile.save(shared_.settings(), err)) LOG_INFO(Config, "Settings from the app saved");
            else LOG_ERROR(Config, "Settings not saved: %s", err.c_str());
        }
        log::global().drain(outputs);
        if (Clock::now() >= nextTelemetry) {
            nextTelemetry += kTelemetryPeriod;
            if (Clock::now() > nextTelemetry) nextTelemetry = Clock::now() + kTelemetryPeriod;
            if (server.clientCount(web::kTelemetryPort)) {
                const std::string p = web::telemetryPacket(shared_.status());
                server.broadcast(web::kTelemetryPort, p.data(), p.size(), true);
            }
        }
    }
    log::global().drain(outputs);  // last words
}

}  // namespace aow
