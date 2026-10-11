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

#include <algorithm>
#include <cerrno>
#include <iterator>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "aow/app_link.h"
#include "aow/gnss_link.h"
#include "aow/indicators.h"
#include "aow/log.h"
#include "aow/motor.h"
#include "aow/steer.h"
#include "board.h"
#include "can_watch.h"
#include "hat_board.h"
#include "isobus.h"
#include "linux_io.h"
#include "shared.h"
#include "web_thread.h"

namespace {

#ifndef AOW_VERSION
#define AOW_VERSION "0.1.0"  // set by CMake (project version, or -DAOW_VERSION=...)
#endif
constexpr const char* kVersion = AOW_VERSION;
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
    uint16_t gvretPort = 23;
    const char* board = "sim";
    const char* canIfaces = "can0,can1,can2";  // CAN1 (K_Bus), CAN2 (ISO_Bus), CAN3 (V_Bus)
    const char* keyaSerial = nullptr;          // RS-232 #2 on the HAT
    const char* radio = nullptr;               // RS-232 #1 on the HAT: RTK radio
    unsigned pwmHz = 20000;                    // --board hat: motor PWM frequency
    float adcRef = 5.0f;                       // --board hat: the measured 5V_MAIN
    bool noWatchdog = false;                   // --board hat: leave the STWD100 off (debugging)
    bool anyApp = false;                       // accept PGNs from any host, not only --app
    bool indicatorTest = false;                // light each LED in turn, play the chime, exit
    bool verbose = false;
};

void usage() {
    std::puts(
        "usage: aow-hatd [options]\n"
        "  --app HOST        where replies go (default 127.0.0.1)\n"
        "  --app-port N      app's port (default 9999)\n"
        "  --any-app         accept PGNs from any host, not only the --app one (bench use)\n"
        "  --listen-port N   port the app sends to (default 8888)\n"
        "  --gnss DEVICE     GNSS receiver serial port, e.g. /dev/ttyUSB0 (default: none)\n"
        "  --gnss-baud N     (default 460800)\n"
        "  --rtcm-port N     RTCM corrections from the app (default 2233)\n"
        "  --config FILE     settings (default /etc/aow-hat/settings.json)\n"
        "  --web-port N      web pages (default 80; 0 = none). WebSockets stay on 8082/8083\n"
        "  --board NAME      hat: the AOW HAT; sim (default): no hardware, the wheel angle\n"
        "                    follows the motor command\n"
        "  --pwm-hz N        hat: motor PWM frequency (default 20000)\n"
        "  --adc-ref V       hat: the measured 5V_MAIN, the ADC reference (default 5.0)\n"
        "  --no-watchdog     hat: leave the hardware watchdog off (for debugging)\n"
        "  --indicator-test  each LED red, green, blue in turn, then the chime; then exit\n"
        "  --can A,B,C       SocketCAN interfaces for CAN1-3 (default can0,can1,can2)\n"
        "  --gvret-port N    SavvyCAN (GVRET) over TCP (default 23; 0 = off)\n"
        "  --keya-serial DEV probe for a Keya motor on this serial port at startup\n"
        "  --radio DEV       RTK radio serial port; its RTCM goes to the receiver\n"
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
        else if (!std::strcmp(a, "--can") && hasValue) o.canIfaces = argv[++i];
        else if (!std::strcmp(a, "--gvret-port") && hasValue) o.gvretPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (!std::strcmp(a, "--keya-serial") && hasValue) o.keyaSerial = argv[++i];
        else if (!std::strcmp(a, "--radio") && hasValue) o.radio = argv[++i];
        else if (!std::strcmp(a, "--pwm-hz") && hasValue) o.pwmHz = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (!std::strcmp(a, "--adc-ref") && hasValue) o.adcRef = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(a, "--no-watchdog")) o.noWatchdog = true;
        else if (!std::strcmp(a, "--any-app")) o.anyApp = true;
        else if (!std::strcmp(a, "--indicator-test")) o.indicatorTest = true;
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

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

void logSummary(const aow::AppLink& app, const aow::GnssLink* gnss, const aow::steer::Autosteer& a,
                const aow::linux_io::UdpLink* udp = nullptr) {
    static uint32_t ignoredSeen = 0;
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
    if (udp && udp->ignored() != ignoredSeen) {
        ignoredSeen = udp->ignored();
        LOG_WARNING(System, "Ignored %u datagrams from other hosts (last %s): another AgOpenGPS app on the network?",
                    ignoredSeen, udp->lastIgnoredHost());
    }
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
    aow::WebThread web(shared, {opt.webPort, opt.gvretPort, kVersion, settingsFile});
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
    if (!opt.anyApp && !udp.setAcceptFrom(opt.appHost)) return fail(1);

    aow::linux_io::SerialPort gnssPort;
    aow::linux_io::UdpLink rtcm;
    if (opt.gnssDevice) {
        if (!gnssPort.open(opt.gnssDevice, opt.gnssBaud)) return fail(1);
        if (!rtcm.open(opt.rtcmPort)) return fail(1);
    }

    aow::linux_io::SerialPort radioPort;
    unsigned radioBaud = shared.settings().gps.radioBaud;
    if (opt.radio) {
        if (radioPort.open(opt.radio, radioBaud)) LOG_INFO(Gnss, "Radio RTCM on %s at %u baud", opt.radio, radioBaud);
        else LOG_ERROR(Gnss, "Radio port %s not opened", opt.radio);
    }

    // The daemon answers from loopback, so the module IP it reports is loopback too.
    const uint8_t moduleIp[4] = {127, 0, 0, 1};
    aow::AppLink app(udp, clock, moduleIp);
    aow::GnssLink gnss(gnssPort, udp, clock);

    aow::SimBoard simBoard;
    aow::hat::HatBoard hatBoard;
    aow::Board* boardPtr = nullptr;
    if (!std::strcmp(opt.board, "sim")) {
        boardPtr = &simBoard;
    } else if (!std::strcmp(opt.board, "hat")) {
        aow::hat::HatOptions ho;
        ho.pwmHz = opt.pwmHz;
        ho.adcReferenceVolts = opt.adcRef;
        if (!hatBoard.open(ho)) return fail(2);
        boardPtr = &hatBoard;
    } else {
        LOG_ERROR(System, "Unknown board '%s' (sim or hat)", opt.board);
        return fail(2);
    }
    aow::Board& board = *boardPtr;
    board.drive({});  // motor off until the controller says otherwise
    LOG_INFO(Autosteer, "Board: %s", board.name());
    if (opt.indicatorTest) {
        // For the bench: confirms the chain order (LED 0 PWR first) and the colour order.
        const aow::indicators::Rgb colours[3] = {{60, 0, 0}, {0, 60, 0}, {0, 0, 60}};
        for (int led = 0; led < 4; ++led) {
            for (const auto& c : colours) {
                aow::indicators::Rgb rgb[4] = {};
                rgb[led] = c;
                board.showLeds(rgb);
                usleep(500'000);
            }
        }
        const aow::indicators::Rgb off[4] = {};
        board.showLeds(off);
        board.playTones(aow::indicators::bootChime(1));
        usleep(1'800'000);
        return fail(0);
    }
    board.playTones(aow::indicators::bootChime(shared.settings().misc.buzzerVolume));

    // CAN buses. The bit rate comes from the settings, applied at startup (a change needs a
    // restart, as in the firmware).
    const aow::Settings startSettings = shared.settings();
    aow::linux_io::SocketCan can[3];
    aow::CanWatch canWatch(shared);
    CanCompatHooks hooks;
    hooks.clock = &clock;
    hooks.popup = [&app](const char* msg, uint8_t seconds, uint8_t color) { app.sendPopup(msg, seconds, color); };
    hooks.mirror = [&shared, &clock](uint8_t bus, const CAN_message_t& msg, bool) {
        if (!shared.gvretListening.load(std::memory_order_relaxed)) return;
        aow::MirrorFrame f;
        f.bus = bus;
        f.id = msg.id;
        f.extended = msg.flags.extended;
        f.len = msg.len > 8 ? 8 : msg.len;
        std::memcpy(f.data, msg.buf, f.len);
        f.micros = static_cast<uint32_t>(clock.nowUs());
        shared.pushMirror(f);
    };
    {
        std::string list = opt.canIfaces;
        for (int i = 0; i < 3; ++i) {
            const size_t comma = list.find(',');
            const std::string name = list.substr(0, comma);
            list = comma == std::string::npos ? "" : list.substr(comma + 1);
            uint32_t bitrate = startSettings.can.bus[i].speed ? 500000 : 250000;
            if (startSettings.isobus.enabled && startSettings.isobus.channel == i + 1 && bitrate != 250000) {
                LOG_WARNING(Can, "CAN%d is the ISOBUS channel: 250 kbit/s, not the 500 set for it", i + 1);
                bitrate = 250000;
            }
            if (!name.empty() && can[i].open(name.c_str(), bitrate)) {
                hooks.bus[i] = &can[i];
                LOG_INFO(Can, "CAN%d on %s, %u bit/s", i + 1, name.c_str(), bitrate);
                canWatch.add(i, name, bitrate);
            }
        }
    }
    canCompatSetup(hooks);
    canWatch.start();

    // ISOBUS shares its channel with the brand engine above: two sockets on one interface.
    aow::Isobus isobus(shared);
    if (startSettings.isobus.enabled) {
        const int ch = startSettings.isobus.channel;
        if (hooks.bus[ch - 1]) {
            // Implements' descriptions and the task controller's log, beside the settings.
            std::string dir = opt.config;
            const size_t slash = dir.rfind('/');
            dir = (slash == std::string::npos ? std::string(".") : dir.substr(0, slash)) + "/isobus";
            isobus.start(can[ch - 1].name(), static_cast<uint8_t>(ch), dir, startSettings.isobus.tractorEcu);
        } else {
            LOG_ERROR(Can, "ISOBUS: CAN%d is not available", ch);
            aow::IsobusStatus s;
            s.state = aow::IsobusStatus::State::Failed;
            s.channel = static_cast<uint8_t>(ch);
            shared.setIsobus(s);
        }
    }

    // The motor, chosen as the firmware's MotorDriverManager: tractor CAN if a brand is set,
    // else a Keya on RS-232 if one answers, else the board's PWM output.
    aow::linux_io::SerialPort keyaPort;
    std::unique_ptr<aow::motor::Motor> motor;
    if (startSettings.can.brand != 0) {
        const std::string path = settingsFile.canConfigPath();
        const std::string json = readFile(path);
        if (!json.empty()) LOG_INFO(Can, "Custom CAN config %s (%zu bytes)", path.c_str(), json.size());
        motor = std::make_unique<aow::motor::TractorCanMotor>(aow::motor::canSteerConfig(startSettings), json);
    } else if (opt.keyaSerial && keyaPort.open(opt.keyaSerial, 115200) &&
               aow::motor::KeyaSerialMotor::probe(keyaPort, clock)) {
        motor = std::make_unique<aow::motor::KeyaSerialMotor>(keyaPort, clock);
    } else {
        motor = std::make_unique<aow::PwmMotor>(board);
    }
    LOG_INFO(Autosteer, "Motor: %s", motor->name());

    aow::steer::Autosteer autosteer(clock, startSettings);
    autosteer.setMotor(motor.get());
    autosteer.setPopup(hooks.popup);
    unsigned settingsSeen = shared.settingsVersion();
    SteerAdapter steerAdapter(autosteer, shared);
    app.setSteerModule(&steerAdapter);
    gnss.setPassThrough(startSettings.gps.passThrough);
    if (opt.gnssDevice) shared.gnssPort = &gnssPort;  // for the UM98x page

    LOG_INFO(Network, "Listening on :%u, replying to %s:%u", opt.listenPort, opt.appHost, opt.appPort);
    if (opt.gnssDevice) LOG_INFO(Gnss, "GNSS on %s at %u baud, RTCM on :%u", opt.gnssDevice, opt.gnssBaud, opt.rtcmPort);
    aow::log::global().setStartupDone();

    // Without GNSS the last two entries hold fd -1, which poll() skips.
    enum { kApp, kTick, kSignal, kGnss, kRtcm, kRadio };
    pollfd fds[] = {{udp.fd(), POLLIN, 0}, {tickFd, POLLIN, 0}, {sigFd, POLLIN, 0},
                    {gnssPort.fd(), POLLIN, 0}, {rtcm.fd(), POLLIN, 0}, {radioPort.fd(), POLLIN, 0}};
    const uint64_t startUs = clock.nowUs();
    aow::indicators::Leds leds(startUs / 1000);
    uint32_t lastRtcmPackets = 0, lastRadioBytes = 0, lastButtonPresses = 0;
    uint64_t lastRadioPulseMs = 0, nextLeds = 0;
    uint8_t ledColors[4] = {};
    const uint64_t summaryTicks = opt.verbose ? 500 : 6000;
    uint64_t ticks = 0, nextSummary = summaryTicks, nextTurnSensor = 0, nextMotorProcess = 0, nextKick = 0;
    bool watchdogOn = false;
    int code = 0;
    for (;;) {
        if (poll(fds, 6, -1) < 0) {
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
        // While the UM98x page has the receiver, RTCM and radio bytes are dropped: they would
        // land in the middle of its commands.
        const bool gnssLent = shared.gnssPaused.load();
        if (fds[kRtcm].revents) {
            uint8_t buf[2048];
            int n;
            while ((n = rtcm.receive(buf, sizeof buf)) > 0)
                if (!gnssLent) gnss.forwardRtcm(buf, static_cast<size_t>(n));
        }
        if (fds[kRadio].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            LOG_ERROR(Gnss, "Radio port %s went away", opt.radio);
            fds[kRadio].fd = -1;
        } else if (fds[kRadio].revents) {
            uint8_t buf[1024];
            int n;
            while ((n = radioPort.read(buf, sizeof buf)) > 0)
                if (opt.gnssDevice && !gnssLent) gnss.forwardRadio(buf, static_cast<size_t>(n));
        }
        if (fds[kTick].revents) {
            uint64_t expirations = 0;
            if (read(tickFd, &expirations, sizeof expirations) == sizeof expirations) ticks += expirations;
            if (!opt.noWatchdog) {
                if (!watchdogOn) {  // from the first tick: the loop is running
                    watchdogOn = true;
                    board.watchdog(true);
                }
                if (ticks >= nextKick) {  // 10 Hz against the 1.6 s timeout
                    nextKick = ticks + 10;
                    board.kickWatchdog();
                }
            }
            // The UM98x page borrowing the GNSS port, or giving it back.
            if (shared.gnssPauseRequested.load() && !shared.gnssPaused.load()) {
                fds[kGnss].fd = -1;  // not polled, so its bytes cannot spin the loop
                shared.gnssPaused = true;
            } else if (!shared.gnssPauseRequested.load() && shared.gnssPaused.load()) {
                fds[kGnss].fd = gnssPort.fd();
                shared.gnssPaused = false;
            }

            // Settings changed on the web page: the controller picks them up here.
            if (shared.settingsVersion() != settingsSeen) {
                settingsSeen = shared.settingsVersion();
                const aow::Settings s = shared.settings();
                autosteer.setSettings(s);
                gnss.setPassThrough(s.gps.passThrough);
                if (opt.radio && s.gps.radioBaud != radioBaud) {  // live, as the firmware
                    radioBaud = s.gps.radioBaud;
                    if (radioPort.setBaud(radioBaud)) LOG_INFO(Gnss, "Radio baud now %u", radioBaud);
                    else LOG_ERROR(Gnss, "Radio baud %u not supported", radioBaud);
                }
            }
            const aow::Settings& settings = autosteer.settings();
            const aow::steer::Output out = autosteer.tick(board.read(settings));
            motor->command(out.enable, out.pwm);
            if (ticks >= nextMotorProcess) {  // 50 Hz, as the firmware's motor task
                nextMotorProcess = ticks + 2;
                motor->process();
            }
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

            // For the ISOBUS task controller: the app's sections, and none once the app is gone.
            shared.isoSectionControl.store(st.haveAppLink && app.sectionControl(), std::memory_order_relaxed);
            shared.isoSections.store(st.haveAppLink ? app.sections() : 0, std::memory_order_relaxed);
            // Speed for the tractor ECU role: the app's (what its section control works from),
            // or GNSS speed with no app.
            const float isoKmh = st.haveAppLink ? app.command().speedKmh : (g.fixQuality ? st.speedKph : 0.0f);
            shared.isoSpeedMmS.store(static_cast<int32_t>(isoKmh / 3.6f * 1000.0f + 0.5f), std::memory_order_relaxed);

            if (ticks >= nextLeds) {  // 10 Hz, as the firmware's LED task
                nextLeds = ticks + 10;
                const uint64_t nowMs = clock.nowUs() / 1000;
                const auto& gc = gnss.counters();
                if (gc.rtcmPackets != lastRtcmPackets) leds.pulseRtcm(nowMs);
                if (gc.radioBytes != lastRadioBytes && nowMs - lastRadioPulseMs >= 1000) {  // radio: once a second
                    leds.pulseRtcm(nowMs);
                    lastRadioPulseMs = nowMs;
                }
                if (autosteer.buttonPresses() != lastButtonPresses) leds.pulseButton(nowMs);
                lastRtcmPackets = gc.rtcmPackets;
                lastRadioBytes = gc.radioBytes;
                lastButtonPresses = autosteer.buttonPresses();

                aow::indicators::Inputs li;
                li.nowMs = nowMs;
                li.networkUp = shared.networkUp.load();
                li.appConnected = app.usSinceAnyFrame() < 5'000'000;
                li.gnssData = g.messageMask && clock.nowUs() - g.lastUpdateUs < 5'000'000;
                li.fixQuality = g.fixQuality;
                li.hasIns = g.hasIns;
                li.insAlignment = g.insAlignment;
                li.motorActive = autosteer.motorState() != aow::steer::MotorState::Disabled;
                leds.update(li);
                aow::indicators::Rgb rgb[4];
                for (int i = 0; i < 4; ++i) {
                    rgb[i] = leds.rgb(static_cast<aow::indicators::Led>(i), settings.misc.ledBrightness, nowMs);
                    ledColors[i] = static_cast<uint8_t>(leds.color(static_cast<aow::indicators::Led>(i)));
                }
                board.showLeds(rgb);
            }
            std::copy(std::begin(ledColors), std::end(ledColors), std::begin(st.leds));
            for (int i = 0; i < 3; ++i) st.canRestarts[i] = shared.canRestarts[i].load(std::memory_order_relaxed);
            shared.setStatus(st);

            if (ticks >= nextSummary) {
                nextSummary = ticks + summaryTicks;
                logSummary(app, opt.gnssDevice ? &gnss : nullptr, autosteer, &udp);
            }
        }
    }

    // Motor off: the CAN or serial motor gets its "disabled" command out before we go.
    motor->command(false, 0);
    motor->process();
    motor->process();
    board.drive({});  // the service unit's ExecStopPost is the backstop
    isobus.stop();
    canWatch.stop();
    if (watchdogOn) board.watchdog(false);
    close(tickFd);
    close(sigFd);
    return fail(code);
}
