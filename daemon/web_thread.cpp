#include "web_thread.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>

#include "api.h"
#include "aow/log.h"
#include "server.h"

namespace aow {

namespace {

using Clock = std::chrono::steady_clock;

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

class DaemonBackend : public web::Backend {
public:
    DaemonBackend(Shared& shared, const SettingsFile& file) : shared_(shared), file_(file) {}

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

    void restart() override {
        LOG_WARNING(System, "Restart requested from the web page");
        // systemd's Restart=always brings the daemon back.
        kill(getpid(), SIGTERM);
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
    Shared& shared_;
    const SettingsFile& file_;
};

}  // namespace

void WebThread::run() {
    // systemd starts the whole process SCHED_FIFO; the web thread must not compete with the
    // control loop.
    sched_param normal{};
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &normal);
    pthread_setname_np(pthread_self(), "aow-web");

    DaemonBackend backend(shared_, opt_.settingsFile);
    web::Server server;
    bool pages = false;
    if (opt_.httpPort) {
        pages = server.listenHttp(opt_.httpPort) != 0;
        pages = server.listenWebSocket(web::kTelemetryPort) != 0 && pages;
        pages = server.listenWebSocket(web::kLogPort) != 0 && pages;
        web::addRoutes(server, backend, opt_.version);
        if (pages) LOG_INFO(Network, "Web pages on port %u", opt_.httpPort);
    }

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
    while (!shared_.stop.load()) {
        server.poll(5);
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
