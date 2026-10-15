// aow-hatd: hardware daemon for the AOW HAT.
//
// Two threads. The control thread (this one, real-time under systemd) runs one poll loop: the
// UDP socket from the app, the GNSS receiver's serial port, RTCM corrections from the app, a
// 100 Hz timer for the control loop, and a signalfd so SIGTERM and SIGINT end the loop cleanly.
// The web thread (web_thread.cpp) serves the pages and writes the log out.
#include <dirent.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <iterator>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "aow/app_link.h"
#include "aow/gnss_link.h"
#include "aow/imu.h"
#include "aow/indicators.h"
#include "aow/log.h"
#include "aow/motor.h"
#include "aow/port_monitor.h"
#include "aow/settings.h"
#include "aow/steer.h"
#include "board.h"
#include "can_watch.h"
#include "hat_board.h"
#include "isobus.h"
#include "linux_io.h"
#include "shared.h"
#include "usb_scout.h"
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
    const char* rs232 = nullptr;               // "DEV1,DEV2": the RS-232 ports; their use is in the settings
    const char* keyaSerial = nullptr;          // these three override the settings' port uses
    const char* radio = nullptr;
    const char* imu = nullptr;
    unsigned pwmHz = 20000;                    // --board hat: motor PWM frequency
    float adcRef = 5.0f;                       // --board hat: the measured 5V_MAIN
    bool noWatchdog = false;                   // --board hat: leave the STWD100 off (debugging)
    bool anyApp = false;                       // accept PGNs from any host, not only --app
    bool indicatorTest = false;                // light each LED in turn, play the chime, exit
    unsigned testHangAfterS = 0;               // watchdog test: the control loop stops after this
    bool verbose = false;
};

void usage() {
    std::puts(
        "usage: aow-hatd [options]\n"
        "  --app HOST        where replies go (default 127.0.0.1)\n"
        "  --app-port N      app's port (default 9999)\n"
        "  --any-app         accept PGNs from any host, not only the --app one (bench use)\n"
        "  --listen-port N   port the app sends to (default 8888)\n"
        "  --gnss DEVICE     the onboard GNSS receiver's serial port (default: none). The\n"
        "                    receiver is used there unless the settings put it elsewhere\n"
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
        "  --test-hang-after S  watchdog test: the control loop stops for good after S seconds,\n"
        "                    the web thread carries on. By hand only, never in daemon.env:\n"
        "                    on the HAT it resets the CM4\n"
        "  --can A,B,C       SocketCAN interfaces for CAN1-3 (default can0,can1,can2)\n"
        "  --gvret-port N    SavvyCAN (GVRET) over TCP (default 23; 0 = off)\n"
        "  --rs232 DEV1,DEV2 the RS-232 ports #1 and #2. Where each serial device is (GNSS\n"
        "                    receiver, RTK radio, IMU, Keya, NMEA out, RTCM out) is a setting,\n"
        "                    on the Serial Communications page\n"
        "  --keya-serial DEV probe for a Keya motor on this serial port at startup\n"
        "  --radio DEV       RTK radio serial port; its RTCM goes to the receiver\n"
        "  --imu DEV         external IMU: a BNO085 (RVC) on an RS-232 port at 115200, or a TM171\n"
        "                    on USB (/dev/ttyACMx; better its /dev/serial/by-id name). Takes the\n"
        "                    port from the radio or the Keya probe if they have the same one;\n"
        "                    reopened when a USB IMU is unplugged and plugged back in.\n"
        "                    These three override the settings' port uses (bench and sim)\n"
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
        else if (!std::strcmp(a, "--rs232") && hasValue) o.rs232 = argv[++i];
        else if (!std::strcmp(a, "--keya-serial") && hasValue) o.keyaSerial = argv[++i];
        else if (!std::strcmp(a, "--radio") && hasValue) o.radio = argv[++i];
        else if (!std::strcmp(a, "--imu") && hasValue) o.imu = argv[++i];
        else if (!std::strcmp(a, "--pwm-hz") && hasValue) o.pwmHz = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (!std::strcmp(a, "--adc-ref") && hasValue) o.adcRef = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(a, "--no-watchdog")) o.noWatchdog = true;
        else if (!std::strcmp(a, "--any-app")) o.anyApp = true;
        else if (!std::strcmp(a, "--indicator-test")) o.indicatorTest = true;
        else if (!std::strcmp(a, "--test-hang-after") && hasValue) o.testHangAfterS = static_cast<unsigned>(std::atoi(argv[++i]));
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
    udp.setOwnAddresses(shared.ownAddresses, aow::Shared::kOwnAddresses);

    // Serial devices: where each is comes from the settings (the Serial Communications page).
    // --gnss names the onboard receiver and --rs232 the two RS-232 ports; --radio,
    // --keya-serial and --imu override the settings.
    using Ports = aow::Settings::Ports;
    const Ports portSettings = shared.settings().ports;
    std::string rs232[2];
    if (opt.rs232) {
        const std::string list = opt.rs232;
        const size_t comma = list.find(',');
        rs232[0] = list.substr(0, comma);
        if (comma != std::string::npos) rs232[1] = list.substr(comma + 1);
    }
    const aow::PortPlan plan = aow::planPorts(portSettings, rs232, opt.gnssDevice ? opt.gnssDevice : "", opt.gnssBaud);
    const aow::PortPlan::Dev& gnssPlan = plan.dev[Ports::kGnss];
    std::string gnssDev = gnssPlan.path;
    const bool usbGnss = gnssPlan.usb;
    std::string radioDev = opt.radio ? opt.radio : plan.dev[Ports::kRadio].path;
    unsigned radioBaud = plan.dev[Ports::kRadio].baud ? plan.dev[Ports::kRadio].baud : portSettings.baud[0];
    std::string keyaDev = opt.keyaSerial ? opt.keyaSerial : plan.dev[Ports::kKeya].path;
    std::string imuDev = opt.imu ? opt.imu : plan.dev[Ports::kImu].path;
    const bool usbImu = !opt.imu && plan.dev[Ports::kImu].usb;
    const std::string nmeaOutDev = plan.dev[Ports::kNmeaOut].path, rtcmOutDev = plan.dev[Ports::kRtcmOut].path;
    // An IMU on a port also named for the radio or the Keya probe (by the options): the IMU has it.
    if (!imuDev.empty() && imuDev == radioDev) {
        LOG_INFO(Imu, "%s is the IMU's: no radio", imuDev.c_str());
        radioDev.clear();
    }
    if (!imuDev.empty() && imuDev == keyaDev) {
        LOG_INFO(Imu, "%s is the IMU's: no Keya serial probe", imuDev.c_str());
        keyaDev.clear();
    }

    aow::linux_io::SerialPort gnssPort;
    aow::linux_io::UdpLink rtcm;
    const bool gnssWanted = !gnssDev.empty() || usbGnss;
    if (!gnssDev.empty() && !gnssPort.open(gnssDev.c_str(), gnssPlan.baud)) return fail(1);
    if (gnssWanted && !rtcm.open(opt.rtcmPort)) return fail(1);

    // USB devices are found by listening to them (usb_scout.h); nothing else in use is tried.
    aow::UsbScout scout;
    for (const std::string& d : {gnssDev, radioDev, keyaDev, imuDev, nmeaOutDev, rtcmOutDev, rs232[0], rs232[1]})
        if (!d.empty()) scout.exclude(d);
    scout.want(aow::UsbScout::Kind::Gnss, usbGnss);
    scout.want(aow::UsbScout::Kind::Imu, usbImu);
    if (usbGnss) LOG_INFO(Gnss, "GNSS receiver on USB: looking for it");
    if (usbImu) LOG_INFO(Imu, "IMU on USB: looking for a TM171");

    aow::linux_io::SerialPort imuPort;
    aow::imu::Imu imu;
    // An IMU on a named port (a USB one named by the --imu option) may appear late or come and
    // go: it is reopened once a second while its device node is there.
    auto openImu = [&]() {
        if (access(imuDev.c_str(), F_OK) != 0 || !imuPort.open(imuDev.c_str(), 115200)) return false;
        LOG_INFO(Imu, "IMU port %s open, waiting for a BNO085 or TM171", imuDev.c_str());
        return true;
    };
    if (!imuDev.empty() && !openImu()) LOG_WARNING(Imu, "IMU port %s not there yet: opened when it appears", imuDev.c_str());

    aow::linux_io::SerialPort radioPort;
    if (!radioDev.empty()) {
        if (radioPort.open(radioDev.c_str(), radioBaud)) LOG_INFO(Gnss, "Radio RTCM on %s at %u baud", radioDev.c_str(), radioBaud);
        else LOG_ERROR(Gnss, "Radio port %s not opened", radioDev.c_str());
    }
    aow::linux_io::SerialPort nmeaOutPort, rtcmOutPort;
    const unsigned nmeaOutBaud = plan.dev[Ports::kNmeaOut].baud, rtcmOutBaud = plan.dev[Ports::kRtcmOut].baud;
    if (!nmeaOutDev.empty()) {
        if (nmeaOutPort.open(nmeaOutDev.c_str(), nmeaOutBaud)) LOG_INFO(Gnss, "NMEA out on %s at %u baud", nmeaOutDev.c_str(), nmeaOutBaud);
        else LOG_ERROR(Gnss, "NMEA out port %s not opened", nmeaOutDev.c_str());
    }
    if (!rtcmOutDev.empty()) {
        if (rtcmOutPort.open(rtcmOutDev.c_str(), rtcmOutBaud)) LOG_INFO(Gnss, "RTCM out on %s at %u baud", rtcmOutDev.c_str(), rtcmOutBaud);
        else LOG_ERROR(Gnss, "RTCM out port %s not opened", rtcmOutDev.c_str());
    }
    // The open port on each RS-232 port whose baud rate is the port's setting (the IMU and Keya
    // run at 115200): a new rate on the Serial Communications page applies live, as the
    // firmware's radio rate did.
    aow::linux_io::SerialPort* onPort[2] = {nullptr, nullptr};
    unsigned portBaud[2] = {portSettings.baud[0], portSettings.baud[1]};
    auto variable = [&](Ports::Device d, aow::linux_io::SerialPort& sp) {
        const int i = plan.dev[d].port;
        if (i >= 0 && sp.fd() >= 0) onPort[i] = &sp;
    };
    if (!opt.radio) variable(Ports::kRadio, radioPort);
    variable(Ports::kGnss, gnssPort);
    variable(Ports::kNmeaOut, nmeaOutPort);
    variable(Ports::kRtcmOut, rtcmOutPort);

    // What is coming in on each RS-232 port, for the Serial Communications page. A port set to
    // nothing is opened to listen (never written to), and so are NMEA and RTCM out; the radio
    // and an IMU there are fed from where they are read.
    aow::PortMonitor portMon[2];
    const char* portUse[2] = {"none", "none"};
    for (int d = 0; d < Ports::kDevices; ++d)
        if (plan.dev[d].port >= 0) portUse[plan.dev[d].port] = Ports::deviceName(static_cast<Ports::Device>(d));
    aow::linux_io::SerialPort sparePort[2];
    aow::linux_io::SerialPort* listenOn[2] = {nullptr, nullptr};
    for (int i = 0; i < 2; ++i) {
        if (!std::strcmp(portUse[i], "nmeaOut")) listenOn[i] = &nmeaOutPort;
        else if (!std::strcmp(portUse[i], "rtcmOut")) listenOn[i] = &rtcmOutPort;
        else if (!std::strcmp(portUse[i], "none") && !rs232[i].empty() && rs232[i] != radioDev && rs232[i] != keyaDev &&
                 rs232[i] != imuDev && sparePort[i].open(rs232[i].c_str(), portSettings.baud[i])) {
            listenOn[i] = &sparePort[i];
            onPort[i] = &sparePort[i];
        }
    }
    const int radioMon = opt.radio ? -1 : plan.dev[Ports::kRadio].port;
    const int imuMon = opt.imu ? -1 : plan.dev[Ports::kImu].port;

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
            isobus.start(can[ch - 1].name(), static_cast<uint8_t>(ch), dir, startSettings.isobus.tractorEcu,
                         startSettings.isobus.terminal);
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
    } else if (!keyaDev.empty() && keyaPort.open(keyaDev.c_str(), 115200) &&
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
    gnss.setImu(&imu);
    imu.setSwapXY(startSettings.steer.useYAxis);
    if (gnssWanted) shared.gnssPort = &gnssPort;  // for the UM98x page
    if (nmeaOutPort.fd() >= 0) gnss.setNmeaOut(&nmeaOutPort, portSettings.nmeaSentences, portSettings.nmeaRateHz);
    if (rtcmOutPort.fd() >= 0) gnss.setRtcmOut(&rtcmOutPort);

    LOG_INFO(Network, "Listening on :%u, replying to %s:%u", opt.listenPort, opt.appHost, opt.appPort);
    if (!gnssDev.empty()) LOG_INFO(Gnss, "GNSS on %s at %u baud, RTCM on :%u", gnssDev.c_str(), gnssPlan.baud, opt.rtcmPort);
    aow::log::global().setStartupDone();

    // Without GNSS the last two entries hold fd -1, which poll() skips.
    enum { kApp, kTick, kSignal, kGnss, kRtcm, kRadio, kImu, kListen0, kListen1, kFds };
    pollfd fds[kFds] = {{udp.fd(), POLLIN, 0},       {tickFd, POLLIN, 0},      {sigFd, POLLIN, 0},
                        {gnssPort.fd(), POLLIN, 0},  {rtcm.fd(), POLLIN, 0},   {radioPort.fd(), POLLIN, 0},
                        {imuPort.fd(), POLLIN, 0},
                        {listenOn[0] ? listenOn[0]->fd() : -1, POLLIN, 0},
                        {listenOn[1] ? listenOn[1]->fd() : -1, POLLIN, 0}};
    const uint64_t startUs = clock.nowUs();
    aow::indicators::Leds leds(startUs / 1000);
    uint32_t lastRtcmPackets = 0, lastRadioBytes = 0, lastButtonPresses = 0;
    uint64_t lastRadioPulseMs = 0, nextLeds = 0;
    uint8_t ledColors[4] = {};
    const uint64_t summaryTicks = opt.verbose ? 500 : 6000;
    uint64_t ticks = 0, nextSummary = summaryTicks, nextTurnSensor = 0, nextMotorProcess = 0, nextKick = 0, nextImuOpen = 0;
    bool watchdogOn = false, isoAppLink = false;
    int code = 0;
    for (;;) {
        if (poll(fds, kFds, -1) < 0) {
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
        if (fds[kApp].revents) {
            app.poll();
            // New section bits go to the ISOBUS thread now, not at the next tick. The tick
            // still does it too: that is what clears them when the app goes away.
            if (isoAppLink) shared.setIsoSections(app.sectionControl(), app.sections());
        }
        if (fds[kGnss].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            // A USB receiver unplugged. Exit; systemd restarts us and we find it again.
            LOG_ERROR(Gnss, "%s went away", gnssDev.c_str());
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
            LOG_ERROR(Gnss, "Radio port %s went away", radioDev.c_str());
            fds[kRadio].fd = -1;
        } else if (fds[kRadio].revents) {
            uint8_t buf[1024];
            int n;
            while ((n = radioPort.read(buf, sizeof buf)) > 0) {
                if (radioMon >= 0) portMon[radioMon].feed(buf, static_cast<size_t>(n), clock.nowUs());
                if (gnssWanted && !gnssLent) gnss.forwardRadio(buf, static_cast<size_t>(n));
            }
        }
        if (fds[kImu].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            LOG_WARNING(Imu, "IMU port %s went away: reopened when it is back", imuDev.c_str());
            imuPort.close();
            fds[kImu].fd = -1;
            if (usbImu) {  // look again: it may come back under another name
                scout.release(imuDev);
                scout.want(aow::UsbScout::Kind::Imu, true);
            }
        } else if (fds[kImu].revents) {
            uint8_t buf[512];
            int n;
            const aow::imu::Type before = imu.type();
            while ((n = imuPort.read(buf, sizeof buf)) > 0) {
                imu.feed(buf, static_cast<size_t>(n), clock.nowUs());
                if (imuMon >= 0) portMon[imuMon].feed(buf, static_cast<size_t>(n), clock.nowUs());
            }
            if (imu.type() != before) {
                LOG_INFO(Imu, "%s detected on %s", aow::imu::typeName(imu.type()), imuDev.c_str());
                app.setImuPresent(true);
            }
        }
        for (int i = 0; i < 2; ++i) {  // ports listened to only for the page
            const int k = kListen0 + i;
            if (fds[k].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                fds[k].fd = -1;
            } else if (fds[k].revents) {
                uint8_t buf[512];
                int n;
                while ((n = listenOn[i]->read(buf, sizeof buf)) > 0) portMon[i].feed(buf, static_cast<size_t>(n), clock.nowUs());
            }
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
            if (!usbImu && !imuDev.empty() && fds[kImu].fd < 0 && ticks >= nextImuOpen) {  // a named IMU coming back
                nextImuOpen = ticks + 100;
                if (openImu()) fds[kImu].fd = imuPort.fd();
            }
            // USB devices being looked for.
            if (scout.wants(aow::UsbScout::Kind::Imu) || scout.wants(aow::UsbScout::Kind::Gnss)) {
                const aow::UsbScout::Kind k = scout.tick(clock.nowUs());
                if (k == aow::UsbScout::Kind::Imu) {
                    scout.want(k, false);
                    imuDev = scout.found();
                    if (openImu()) {
                        fds[kImu].fd = imuPort.fd();
                    } else {
                        scout.release(imuDev);
                        scout.want(k, true);
                    }
                } else if (k == aow::UsbScout::Kind::Gnss) {
                    scout.want(k, false);
                    gnssDev = scout.found();
                    if (gnssPort.open(gnssDev.c_str(), scout.foundBaud())) {
                        LOG_INFO(Gnss, "GNSS receiver found on %s", gnssDev.c_str());
                        if (!shared.gnssPaused.load()) fds[kGnss].fd = gnssPort.fd();
                    } else {
                        scout.release(gnssDev);
                        scout.want(k, true);
                    }
                }
            }
            // --test-hang-after: a hung control loop, for the hardware watchdog to catch. After
            // the kick, so the timeout runs from here; the outputs stay as they are.
            if (opt.testHangAfterS && ticks >= opt.testHangAfterS * 100ULL) {
                LOG_ERROR(System, "Test: control loop hung on purpose (--test-hang-after %u)", opt.testHangAfterS);
                for (;;) pause();  // SIGTERM and SIGINT are blocked: only a reset or SIGKILL ends it
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
                imu.setSwapXY(s.steer.useYAxis);
                gnss.setPassThrough(s.gps.passThrough);
                for (int i = 0; i < 2; ++i) {  // RS-232 rates: live, as the firmware's radio rate
                    if (!onPort[i] || s.ports.baud[i] == portBaud[i]) continue;
                    portBaud[i] = s.ports.baud[i];
                    if (onPort[i]->setBaud(portBaud[i])) LOG_INFO(Gnss, "RS-232 port %d now %u baud", i + 1, portBaud[i]);
                    else LOG_ERROR(Gnss, "RS-232 port %d: %u baud not supported", i + 1, portBaud[i]);
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
                // PGN 240, as AOG-TaskController: the task controller is alive, and what the
                // implement reports. Nothing is sent when ISOBUS is off or not on the bus.
                if (shared.isoTcRunning.load(std::memory_order_relaxed))
                    app.sendSectionStatus(shared.isoTcControl.load(std::memory_order_relaxed),
                                          shared.isoTcSections.load(std::memory_order_relaxed),
                                          shared.isoTcActual.load(std::memory_order_relaxed));
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
            st.imuType = aow::imu::typeName(imu.type());
            st.imuValid = imu.valid(clock.nowUs());
            if (fds[kImu].fd >= 0) std::snprintf(st.imuPort, sizeof st.imuPort, "%s", imuDev.c_str());
            if (gnssPort.fd() >= 0) std::snprintf(st.gnssPort, sizeof st.gnssPort, "%s", gnssDev.c_str());
            st.radioBytes = gnss.counters().radioBytes;
            st.nmeaOutSent = gnss.counters().nmeaOutSent;
            st.rtcmOutBytes = gnss.counters().rtcmOutBytes;
            for (int i = 0; i < 2; ++i) {
                aow::Status::Rs232& v = st.rs232[i];
                const aow::PortMonitor& m = portMon[i];
                v.use = portUse[i];
                v.open = !rs232[i].empty() && (fds[kListen0 + i].fd >= 0 || i == radioMon || i == imuMon ||
                                               plan.dev[Ports::kGnss].port == i || plan.dev[Ports::kKeya].port == i);
                v.bytesIn = m.bytes();
                if (m.lastUs()) v.msSinceIn = static_cast<uint32_t>((clock.nowUs() - m.lastUs()) / 1000);
                v.lineCount = m.lineCount();
                for (int l = 0; l < m.lineCount(); ++l) std::memcpy(v.lines[l], m.line(l), sizeof v.lines[l]);
                v.rtcmCount = m.rtcmTypes(clock.nowUs(), v.rtcm, aow::PortMonitor::kRtcmTypes);
            }
            st.imuHeading = imu.data().heading;
            st.imuRoll = imu.data().roll;
            st.imuPitch = imu.data().pitch;
            st.imuYawRate = imu.data().yawRate;

            // For the ISOBUS task controller: the app's sections, and none once the app is gone.
            isoAppLink = st.haveAppLink;
            shared.isoAppLink.store(st.haveAppLink, std::memory_order_relaxed);
            shared.isoXteMm.store(st.haveAppLink ? app.command().xte * 10 : 0, std::memory_order_relaxed);
            shared.setIsoSections(st.haveAppLink && app.sectionControl(), st.haveAppLink ? app.sections() : 0);
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
                logSummary(app, gnssWanted ? &gnss : nullptr, autosteer, &udp);
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
