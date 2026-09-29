// aow-hatd: hardware daemon for the AOW HAT.
//
// Two threads. The control thread (this one, real-time under systemd) runs one poll loop: the
// UDP socket from the app, the GNSS receiver's serial port, RTCM corrections from the app, a
// 100 Hz timer for the control loop, and a signalfd so SIGTERM and SIGINT end the loop cleanly.
// The web thread (web_thread.cpp) serves the pages and writes the log out.
#include <poll.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "aow/app_link.h"
#include "aow/gnss_link.h"
#include "aow/log.h"
#include "aow/steer.h"
#include "board.h"
#include "linux_io.h"
#include "shared.h"
#include "web_thread.h"

namespace {

constexpr const char* kVersion = "0.1.0";
constexpr long kTickNs = 10'000'000;           // 100 Hz, the rate the app sends PGN 254
constexpr uint64_t kAppLinkTimeoutUs = 2'000'000;  // the firmware's steer watchdog

struct Options {
    const char* appHost = "127.0.0.1";
    uint16_t appPort = 9999;
    uint16_t listenPort = 8888;
    const char* gnssDevice = nullptr;  // none: no GNSS
    unsigned gnssBaud = 460800;
    uint16_t rtcmPort = 2233;
    const char* config = "/etc/aow-hat/settings.json";
    uint16_t webPort = 80;
    const char* board = "sim";
    bool verbose = false;
};

void usage() {
    std::puts(
        "usage: aow-hatd [options]\n"
        "  --app HOST        where replies go (default 127.0.0.1)\n"
        "  --app-port N      app's port (default 9999)\n"
        "  --listen-port N   port the app sends to (default 8888)\n"
        "  --gnss DEVICE     GNSS receiver serial port, e.g. /dev/ttyUSB0 (default: none)\n"
        "  --gnss-baud N     (default 460800)\n"
        "  --rtcm-port N     RTCM corrections from the app (default 2233)\n"
        "  --config FILE     settings (default /etc/aow-hat/settings.json)\n"
        "  --web-port N      web pages (default 80; 0 = none). WebSockets stay on 8082/8083\n"
        "  --board NAME      sim (default until the HAT drivers exist): no hardware, the\n"
        "                    wheel angle follows the motor command\n"
        "  -v, --verbose     status summary every 5 s instead of every 60 s\n"
        "  --version");
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const bool hasValue = i + 1 < argc;
        if (!std::strcmp(a, "--app") && hasValue) o.appHost = argv[++i];
        else if (!std::strcmp(a, "--app-port") && hasValue) o.appPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (!std::strcmp(a, "--listen-port") && hasValue) o.listenPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (!std::strcmp(a, "--gnss") && hasValue) o.gnssDevice = argv[++i];
        else if (!std::strcmp(a, "--gnss-baud") && hasValue) o.gnssBaud = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (!std::strcmp(a, "--rtcm-port") && hasValue) o.rtcmPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (!std::strcmp(a, "--config") && hasValue) o.config = argv[++i];
        else if (!std::strcmp(a, "--web-port") && hasValue) o.webPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (!std::strcmp(a, "--board") && hasValue) o.board = argv[++i];
        else if (!std::strcmp(a, "-v") || !std::strcmp(a, "--verbose")) o.verbose = true;
        else if (!std::strcmp(a, "--version")) { std::printf("aow-hatd %s\n", kVersion); std::exit(0); }
        else { usage(); return false; }
    }
    return true;
}

// Blocks SIGTERM/SIGINT in this thread and every thread started after, and returns an fd that
// reads them. Call before starting the web thread.
int openSignalFd() {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    if (sigprocmask(SIG_BLOCK, &set, nullptr) < 0) return -1;
    return signalfd(-1, &set, SFD_CLOEXEC);
}

int openTickTimer() {
    const int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (fd < 0) return -1;
    itimerspec spec{};
    spec.it_interval.tv_nsec = kTickNs;
    spec.it_value.tv_nsec = kTickNs;
    if (timerfd_settime(fd, 0, &spec, nullptr) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Hands the app's steering PGNs to the autosteer, and settings it changes to the web thread
// to save: the control thread does no file I/O.
class SteerAdapter : public aow::SteerModule {
public:
    SteerAdapter(aow::steer::Autosteer& a, aow::Shared& shared) : a_(a), shared_(shared) {}
    void onSteerData(const aow::pgn::SteerCommand& c) override { a_.onSteerData(c); }
    void onSteerConfig(const uint8_t* d, size_t n) override { if (a_.onSteerConfig(d, n)) publish(); }
    void onSteerSettings(const uint8_t* d, size_t n) override { if (a_.onSteerSettings(d, n)) publish(); }
    aow::pgn::SteerStatus steerStatus() override { return a_.status(); }

private:
    void publish() {
        shared_.setSettings(a_.settings());
        shared_.requestSave();
    }
    aow::steer::Autosteer& a_;
    aow::Shared& shared_;
};

void logSummary(const aow::AppLink& app, const aow::GnssLink* gnss, const aow::steer::Autosteer& a) {
    LOG_INFO(Autosteer, "Steer: %s, guidance %s, motor %s, WAS %.1f, target %.1f, speed %.1f", a.armed() ? "armed" : "disarmed",
             a.guidanceActive() ? "on" : "off", a.motorState() == aow::steer::MotorState::Disabled ? "off" : "on",
             static_cast<double>(a.actualAngleDeg()), static_cast<double>(a.targetAngleDeg()),
             static_cast<double>(a.speedKmh()));
    const auto& c = app.counters();
    const uint64_t age = app.usSinceCommand();
    if (age == UINT64_MAX)
        LOG_INFO(System, "App: rx %u, rejected %u, no PGN 254 yet", c.received, c.rejected);
    else
        LOG_INFO(System, "App: rx %u, rejected %u, PGN 254 %u, last %llu ms ago, send errors %u", c.received,
                 c.rejected, c.commands, static_cast<unsigned long long>(age / 1000), c.sendErrors);
    if (gnss) {
        const auto& g = gnss->counters();
        const auto& s = gnss->state();
        LOG_INFO(Gnss, "GNSS: sentences %u, bad checksum %u, nav sent %u, fix %u, sats %u, RTCM %u (dropped %u B)",
                 g.sentences, gnss->reader().badChecksums(), g.navSent, s.fixQuality, s.numSatellites,
                 g.rtcmPackets, g.rtcmBytesDropped);
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) return 2;

    aow::Shared shared;
    aow::SettingsFile settingsFile{opt.config};
    {
        aow::Settings s;
        std::string err;
        if (!settingsFile.load(s, err)) LOG_ERROR(Config, "Settings: %s; using defaults", err.c_str());
        shared.setSettings(s);
        aow::log::global().setConfig(s.log);
    }

    const int sigFd = openSignalFd();
    const int tickFd = openTickTimer();

    // The web thread also writes the log out, so it starts before anything can fail.
    aow::WebThread web(shared, {opt.webPort, kVersion, settingsFile});
    web.start();
    auto fail = [&](int code) {
        shared.stop = true;
        web.join();
        return code;
    };

    LOG_NOTICE(System, "aow-hatd %s starting", kVersion);
    if (sigFd < 0 || tickFd < 0) {
        LOG_ERROR(System, "signalfd/timerfd: %s", std::strerror(errno));
        return fail(1);
    }
    // Keep the control loop out of page faults. Fails harmlessly without the privilege.
    if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0) LOG_WARNING(System, "mlockall: %s (continuing)", std::strerror(errno));

    aow::linux_io::MonotonicClock clock;
    aow::linux_io::UdpLink udp;
    if (!udp.open(opt.listenPort) || !udp.setDestination(opt.appHost, opt.appPort)) return fail(1);

    aow::linux_io::SerialPort gnssPort;
    aow::linux_io::UdpLink rtcm;
    if (opt.gnssDevice) {
        if (!gnssPort.open(opt.gnssDevice, opt.gnssBaud)) return fail(1);
        if (!rtcm.open(opt.rtcmPort)) return fail(1);
    }

    // The daemon answers from loopback, so the module IP it reports is loopback too.
    const uint8_t moduleIp[4] = {127, 0, 0, 1};
    aow::AppLink app(udp, clock, moduleIp);
    aow::GnssLink gnss(gnssPort, udp, clock);

    if (std::strcmp(opt.board, "sim") != 0) {
        LOG_ERROR(System, "Unknown board '%s' (only 'sim' exists until the HAT drivers are written)", opt.board);
        return fail(2);
    }
    aow::SimBoard simBoard;
    aow::Board& board = simBoard;
    aow::steer::Autosteer autosteer(clock, shared.settings());
    unsigned settingsSeen = shared.settingsVersion();
    SteerAdapter steerAdapter(autosteer, shared);
    app.setSteerModule(&steerAdapter);
    board.drive({});  // motor off until the controller says otherwise
    LOG_INFO(Autosteer, "Board: %s", board.name());

    LOG_INFO(Network, "Listening on :%u, replying to %s:%u", opt.listenPort, opt.appHost, opt.appPort);
    if (opt.gnssDevice) LOG_INFO(Gnss, "GNSS on %s at %u baud, RTCM on :%u", opt.gnssDevice, opt.gnssBaud, opt.rtcmPort);
    aow::log::global().setStartupDone();

    // Without GNSS the last two entries hold fd -1, which poll() skips.
    enum { kApp, kTick, kSignal, kGnss, kRtcm };
    pollfd fds[] = {{udp.fd(), POLLIN, 0}, {tickFd, POLLIN, 0}, {sigFd, POLLIN, 0},
                    {gnssPort.fd(), POLLIN, 0}, {rtcm.fd(), POLLIN, 0}};
    const uint64_t startUs = clock.nowUs();
    const uint64_t summaryTicks = opt.verbose ? 500 : 6000;
    uint64_t ticks = 0, nextSummary = summaryTicks, nextTurnSensor = 0;
    int code = 0;
    for (;;) {
        if (poll(fds, 5, -1) < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR(System, "poll: %s", std::strerror(errno));
            code = 1;
            break;
        }
        if (fds[kSignal].revents) {
            signalfd_siginfo si;
            if (read(sigFd, &si, sizeof si) == sizeof si) LOG_NOTICE(System, "Signal %u: stopping", si.ssi_signo);
            break;
        }
        if (fds[kApp].revents) app.poll();
        if (fds[kGnss].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            // A USB receiver unplugged. Exit; systemd restarts us and we reopen it.
            LOG_ERROR(Gnss, "%s went away", opt.gnssDevice);
            code = 1;
            break;
        }
        if (fds[kGnss].revents) gnss.pollReceiver();
        if (fds[kRtcm].revents) {
            uint8_t buf[2048];
            int n;
            while ((n = rtcm.receive(buf, sizeof buf)) > 0) gnss.forwardRtcm(buf, static_cast<size_t>(n));
        }
        if (fds[kTick].revents) {
            uint64_t expirations = 0;
            if (read(tickFd, &expirations, sizeof expirations) == sizeof expirations) ticks += expirations;
            // Settings changed on the web page: the controller picks them up here.
            if (shared.settingsVersion() != settingsSeen) {
                settingsSeen = shared.settingsVersion();
                autosteer.setSettings(shared.settings());
            }
            const aow::Settings& settings = autosteer.settings();
            board.drive(autosteer.tick(board.read(settings)));
            if (ticks >= nextTurnSensor) {  // PGN 250, 10 Hz
                nextTurnSensor = ticks + 10;
                app.sendTurnSensor(autosteer.turnSensorReading());
            }

            aow::Status st;
            st.uptimeMs = (clock.nowUs() - startUs) / 1000;
            st.haveAppLink = app.usSinceCommand() < kAppLinkTimeoutUs;
            st.wasAngle = autosteer.wasAngleDeg();
            st.targetAngle = autosteer.targetAngleDeg();
            st.currentAmps = autosteer.currentCounts() / 1000.0f;  // as the firmware's telemetry
            st.autosteerActive = autosteer.motorState() != aow::steer::MotorState::Disabled;
            st.steerSwitch = autosteer.steerSwitch();
            st.workSwitch = autosteer.workSwitch();
            const auto& g = gnss.state();
            st.speedKph = g.speedKnots * 1.852f;  // telemetry speed and heading come from GNSS
            st.heading = g.headingTrue;
            st.fixQuality = g.fixQuality;
            st.satellites = g.numSatellites;
            st.navSent = gnss.counters().navSent;
            shared.setStatus(st);

            if (ticks >= nextSummary) {
                nextSummary = ticks + summaryTicks;
                logSummary(app, opt.gnssDevice ? &gnss : nullptr, autosteer);
            }
        }
    }

    board.drive({});  // motor off; the service unit's ExecStopPost is the backstop
    close(tickFd);
    close(sigFd);
    return fail(code);
}
