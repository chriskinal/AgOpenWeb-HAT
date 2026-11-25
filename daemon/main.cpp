// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
// Ported from Firmware_Teensy_AiO-New-Dawn, copyright 2025 by the AOG Group.
//
// aow-hatd: hardware daemon for the AOW HAT.
//
// Two threads. The control thread (this one, real-time under systemd) runs one poll loop: the
// UDP socket from the app, the GNSS receiver's serial port, RTCM corrections from the app, a
// 100 Hz timer for the control loop, and a signalfd so SIGTERM and SIGINT end the loop cleanly.
// The web thread (web_thread.cpp) serves the pages and writes the log out.
#include <dirent.h>
#include <poll.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sched.h>
#include <sys/utsname.h>
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
#include "aow/settings.h"
#include "aow/steer.h"
#include "board.h"
#include "can_watch.h"
#include "hat_board.h"
#include "isobus.h"
#include "linux_io.h"
#include "shared.h"
#include "serial_hub.h"
#include "web_thread.h"

namespace {

#ifndef AOW_VERSION
#define AOW_VERSION "0.1.0"  // set by CMake (project version, or -DAOW_VERSION=...)
#endif
constexpr const char* kVersion = AOW_VERSION;
constexpr long kTickNs = 10'000'000;           // 100 Hz, the rate the app sends PGN 254
constexpr uint64_t kAppLinkTimeoutUs = 2'000'000;  // the firmware's steer watchdog
constexpr uint64_t kImuGoneUs = 2'000'000;  // no IMU packet for this long: the IMU is gone

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
    bool noWatchdog = false;
    bool usb = false;                          // look for USB serial devices (on with --board hat)                   // --board hat: leave the STWD100 off (debugging)
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
        "  --gnss DEVICE     the onboard GNSS receiver's serial port (default: none)\n"
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
        "  --rs232 DEV1,DEV2 the RS-232 ports #1 and #2. What is plugged in there, or into USB, is\n"
        "                    found from what it sends (a receiver, RTK radio or IMU); the outputs\n"
        "                    and rates are set on the Serial Communications page\n"
        "  --keya-serial DEV probe for a Keya motor on this serial port at startup\n"
        "  --radio DEV       an RTK radio on this serial port, taken as such (bench)\n"
        "  --imu DEV         an IMU on this serial port, taken as such (bench): a BNO085 (RVC)\n"
        "                    at 115200 or a TM171; reopened when it is unplugged and plugged back in\n"
        "  --usb             look for USB serial devices (a receiver, radio or IMU) with\n"
        "                    --board sim too; with --board hat always\n"
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
        else if (!std::strcmp(a, "--usb")) o.usb = true;
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

// Every thread the daemon starts, AgIsoStack++'s included, gets this stack instead of glibc's
// 8 MB. mlockall(MCL_FUTURE) makes a whole stack resident as the thread starts, so seven 8 MB
// stacks were 56 of the daemon's 66 MB. The deepest seen on the dev CM4 after hours of use was
// 36 kB (the web thread); the rest touch 12 kB. Set before the first thread starts.
constexpr std::size_t kThreadStack = 512 * 1024;

void setThreadStackSize() {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, kThreadStack);
    pthread_setattr_default_np(&a);
    pthread_attr_destroy(&a);
}

// Keeps the control loop out of page faults: every page mapped now or later is made resident
// and locked. Locking pages only as they were touched (MCL_ONFAULT) saved the same memory but
// let other threads take page faults while holding a lock the loop needs: ticks up to 2.7 ms
// late, 11 in 4.5 hours, against none in 28 hours with everything resident. The control
// thread's stack grows on demand even so, so 256 kB of it is touched now.
// Returns false if the lock failed (no privilege); the daemon carries on.
bool lockMemory() {
    if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0) return false;
    constexpr std::size_t kStackPrefault = 256 * 1024;
    volatile char stack[kStackPrefault];
    for (std::size_t i = 0; i < kStackPrefault; i += 4096) stack[i] = 0;
    (void)stack[0];
    return true;
}

// Fills in how this (the control) thread is scheduled and logs it once. The service unit starts
// the process FIFO 80; run by hand, or on a kernel without PREEMPT_RT, the loop still runs but
// its timing is not to be trusted, so on the HAT that is a warning. The Pi's RT kernel has no
// /sys/kernel/realtime; its version string says PREEMPT_RT.
void reportLoopSetup(aow::LoopSetup& s, bool warn) {
    const int policy = sched_getscheduler(0);
    s.policy = policy == SCHED_FIFO ? "fifo" : policy == SCHED_RR ? "rr" : "other";
    sched_param sp{};
    if (sched_getparam(0, &sp) == 0) s.priority = sp.sched_priority;
    utsname u{};
    s.rtKernel = uname(&u) == 0 && std::strstr(u.version, "PREEMPT_RT");
    const bool realtime = s.priority > 0 && s.rtKernel && s.memoryLocked;
    if (realtime || !warn) {
        LOG_NOTICE(System, "Control loop: %s %d, %s kernel, memory %slocked", s.policy, s.priority,
                   s.rtKernel ? "PREEMPT_RT" : "non-RT", s.memoryLocked ? "" : "not ");
    } else {
        LOG_WARNING(System, "Control loop not real-time: %s %d, %s kernel, memory %slocked (expected fifo 80, PREEMPT_RT, locked)",
                    s.policy, s.priority, s.rtKernel ? "PREEMPT_RT" : "non-RT", s.memoryLocked ? "" : "not ");
    }
}

// How long poll() may wait for the tick due at dueUs: zero once it is due. Rounded up to the
// microsecond, so the wait never ends before the tick.
timespec untilTick(uint64_t nowUs, uint64_t dueUs) {
    timespec t{};
    if (dueUs > nowUs) {
        const uint64_t us = dueUs - nowUs;
        t.tv_sec = static_cast<time_t>(us / 1'000'000);
        t.tv_nsec = static_cast<long>(us % 1'000'000) * 1000;
    }
    return t;
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

// Hands the app's vehicle geometry to the web thread, which sets the receiver's INS lever arm.
class GeometryAdapter : public aow::GeometrySink {
public:
    explicit GeometryAdapter(aow::Shared& shared) : shared_(shared) {}
    void onVehicleGeometry(const aow::pgn::VehicleGeometry& g) override { shared_.setGeometry(g); }

private:
    aow::Shared& shared_;
};

// The app's corrected position (PGN 100) to NMEA out.
class FixAdapter : public aow::FixSink {
public:
    explicit FixAdapter(aow::GnssLink& g) : g_(g) {}
    void onCorrectedFix(const aow::pgn::CorrectedFix& f) override { g_.onCorrectedFix(f); }

private:
    aow::GnssLink& g_;
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
    setThreadStackSize();

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

    // The web thread also writes the log out, so it starts before anything can fail.
    aow::WebThread web(shared, {opt.webPort, opt.gvretPort, kVersion, settingsFile});
    web.start();
    auto fail = [&](int code) {
        shared.stop = true;
        web.join();
        return code;
    };

    LOG_NOTICE(System, "aow-hatd %s starting", kVersion);
    if (sigFd < 0) {
        LOG_ERROR(System, "signalfd: %s", std::strerror(errno));
        return fail(1);
    }
    // Keep the control loop out of page faults. Fails harmlessly without the privilege.
    aow::LoopSetup loopSetup;
    loopSetup.memoryLocked = lockMemory();
    if (!loopSetup.memoryLocked) LOG_WARNING(System, "mlockall: %s (continuing)", std::strerror(errno));
    reportLoopSetup(loopSetup, !std::strcmp(opt.board, "hat"));

    aow::linux_io::MonotonicClock clock;
    aow::linux_io::UdpLink udp;
    if (!udp.open(opt.listenPort) || !udp.setDestination(opt.appHost, opt.appPort)) return fail(1);
    if (!opt.anyApp && !udp.setAcceptFrom(opt.appHost)) return fail(1);
    udp.setOwnAddresses(shared.ownAddresses, aow::Shared::kOwnAddresses);

    // Serial devices (serial_hub.h): the onboard receiver (--gnss), the RS-232 ports (--rs232)
    // and USB serial devices, what is on them found by listening. --radio and --imu name devices
    // taken as such; a Keya motor's port (settings file, or --keya-serial) is left to it.
    using Ports = aow::Settings::Ports;
    const Ports portSettings = shared.settings().ports;
    std::string rs232[2];
    if (opt.rs232) {
        const std::string list = opt.rs232;
        const size_t comma = list.find(',');
        rs232[0] = list.substr(0, comma);
        if (comma != std::string::npos) rs232[1] = list.substr(comma + 1);
    }
    const std::string keyaDev = opt.keyaSerial                         ? opt.keyaSerial
                              : portSettings.keya == Ports::kPort1 ? rs232[0]
                              : portSettings.keya == Ports::kPort2 ? rs232[1]
                                                                   : std::string();
    aow::linux_io::UdpLink rtcm;
    if (!rtcm.open(opt.rtcmPort)) return fail(1);
    aow::imu::Imu imu;

    // The daemon answers from loopback, so the module IP it reports is loopback too.
    const uint8_t moduleIp[4] = {127, 0, 0, 1};
    aow::AppLink app(udp, clock, moduleIp);
    aow::linux_io::SerialPort noReceiver;  // GnssLink's until the hub names the receiver in use
    aow::GnssLink gnss(noReceiver, udp, clock);
    aow::SerialHub::Config hubConfig;
    if (opt.gnssDevice) hubConfig.onboard = opt.gnssDevice;
    hubConfig.onboardBaud = opt.gnssBaud;
    hubConfig.rs232[0] = rs232[0];
    hubConfig.rs232[1] = rs232[1];
    if (opt.radio) hubConfig.radio = opt.radio;
    hubConfig.radioBaud = portSettings.rate[0] ? portSettings.rate[0] : 115200;
    if (opt.imu) hubConfig.imu = opt.imu;
    hubConfig.keya = keyaDev;
    // Not with --board sim unless asked: a test daemon beside the service would read the
    // service's USB devices too, and the two would share their bytes.
    hubConfig.usb = opt.usb || !std::strcmp(opt.board, "hat");
    aow::SerialHub hub(hubConfig, gnss, imu, clock);
    hub.apply(portSettings);

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
    uint64_t imuSeenUs = 0;  // the last valid IMU packet
    bool imuAnswered = false;  // answering AgOpenWeb's hello as the IMU
    SteerAdapter steerAdapter(autosteer, shared);
    app.setSteerModule(&steerAdapter);
    FixAdapter fixAdapter(gnss);
    app.setFixSink(&fixAdapter);
    GeometryAdapter geometryAdapter(shared);
    app.setGeometrySink(&geometryAdapter);
    gnss.setNmeaCorrected(portSettings.nmeaCorrected);
    gnss.setPassThrough(startSettings.gps.passThrough);
    gnss.setImu(&imu);
    imu.setSwapXY(startSettings.steer.useYAxis);
    shared.gnssPort = hub.onboardPort();  // for the UM98x page

    LOG_INFO(Network, "Listening on :%u, replying to %s:%u", opt.listenPort, opt.appHost, opt.appPort);
    if (opt.gnssDevice) LOG_INFO(Gnss, "GNSS on %s at %u baud, RTCM on :%u", opt.gnssDevice, opt.gnssBaud, opt.rtcmPort);
    aow::log::global().setStartupDone();

    // The serial ports are read on the hub's reader thread: the loop polls its eventfd, never a
    // port (serial_reader.h: polling a tty can wait on an ordinary kworker).
    // The tick is poll()'s timeout, not a timerfd: on PREEMPT_RT a timerfd expires in ktimers,
    // a FIFO 1 thread, so any interrupt thread (or our serial reader) on that CPU held the tick
    // back, up to 1.1 ms on the dev CM4; a FIFO thread's poll() timeout expires in the timer
    // interrupt itself (docs/PLAN.md, 2026-11-19).
    enum { kApp, kSignal, kRtcm, kSerial, kCount };
    pollfd fds[kCount];
    const uint64_t startUs = clock.nowUs();
    aow::indicators::Leds leds(startUs / 1000);
    uint32_t lastRtcmPackets = 0, lastRadioBytes = 0, lastButtonPresses = 0;
    uint64_t lastRadioPulseMs = 0, nextLeds = 0;
    uint8_t ledColors[4] = {};
    const uint64_t summaryTicks = opt.verbose ? 500 : 6000;
    uint64_t ticks = 0, nextSummary = summaryTicks, nextTurnSensor = 0, nextMotorProcess = 0, nextKick = 0;
    bool watchdogOn = false, isoAppLink = false;
    int code = 0;
    // Tick n is due n periods after here; each tick's lateness is measured from that.
    constexpr uint64_t kTickUs = kTickNs / 1000;
    aow::LoopTiming loopTiming(kTickUs, 1000);  // worst cases over 10 s windows
    const uint64_t tickStartUs = clock.nowUs();
    loopTiming.start(tickStartUs);
    for (;;) {
        fds[kApp] = {udp.fd(), POLLIN, 0};
        fds[kSignal] = {sigFd, POLLIN, 0};
        fds[kRtcm] = {rtcm.fd(), POLLIN, 0};
        fds[kSerial] = {hub.wakeFd(), POLLIN, 0};
        const timespec wait = untilTick(clock.nowUs(), tickStartUs + (ticks + 1) * kTickUs);
        if (ppoll(fds, kCount, &wait, nullptr) < 0) {
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
        // While the UM98x page has the receiver, RTCM and radio bytes are dropped: they would
        // land in the middle of its commands.
        const bool gnssLent = shared.gnssPaused.load();
        if (fds[kRtcm].revents) {
            uint8_t buf[2048];
            int n;
            while ((n = rtcm.receive(buf, sizeof buf)) > 0)
                if (!gnssLent) gnss.forwardRtcm(buf, static_cast<size_t>(n));
        }
        if (fds[kSerial].revents) {
            const aow::imu::Type before = imu.type();
            hub.handle();
            if (imu.type() != before && imu.type() != aow::imu::Type::None)
                LOG_INFO(Imu, "%s detected on %s", aow::imu::typeName(imu.type()), hub.imuName().c_str());
        }
        const uint64_t tickBeginUs = clock.nowUs();
        if (tickBeginUs >= tickStartUs + (ticks + 1) * kTickUs) {
            ticks = (tickBeginUs - tickStartUs) / kTickUs;  // more than one up: ticks were missed
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
            hub.tick();
            // --test-hang-after: a hung control loop, for the hardware watchdog to catch. After
            // the kick, so the timeout runs from here; the outputs stay as they are.
            if (opt.testHangAfterS && ticks >= opt.testHangAfterS * 100ULL) {
                LOG_ERROR(System, "Test: control loop hung on purpose (--test-hang-after %u)", opt.testHangAfterS);
                for (;;) pause();  // SIGTERM and SIGINT are blocked: only a reset or SIGKILL ends it
            }
            // The UM98x page borrowing the GNSS port, or giving it back.
            if (shared.gnssPauseRequested.load() && !shared.gnssPaused.load()) {
                hub.setLent(true);  // not read, so its bytes cannot spin the loop
                if (hub.lentReady()) shared.gnssPaused = true;  // the reader has let the port go
            } else if (!shared.gnssPauseRequested.load() && shared.gnssPaused.load()) {
                hub.setLent(false);
                shared.gnssPaused = false;
            }

            // Settings changed on the web page: the controller picks them up here.
            if (shared.settingsVersion() != settingsSeen) {
                settingsSeen = shared.settingsVersion();
                const aow::Settings s = shared.settings();
                autosteer.setSettings(s);
                imu.setSwapXY(s.steer.useYAxis);
                gnss.setPassThrough(s.gps.passThrough);
                hub.apply(s.ports);  // outputs and rates, live
                gnss.setNmeaCorrected(s.ports.nmeaCorrected);
            }
            const aow::Settings& settings = autosteer.settings();
            const uint64_t inputsBeginUs = clock.nowUs();
            const aow::steer::Inputs inputs = board.read(settings);
            const uint64_t inputsEndUs = clock.nowUs();
            const aow::steer::Output out = autosteer.tick(inputs);
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
            st.gnssIns = g.hasIns;
            st.navSent = gnss.counters().navSent;
            st.imuType = aow::imu::typeName(imu.type());
            st.imuValid = imu.valid(clock.nowUs());
            // AgOpenWeb sees an IMU while the HAT answers its hello as 121: from the first packet
            // until kImuGoneUs without one (unplugged), so its IMU light goes out too.
            if (st.imuValid) imuSeenUs = clock.nowUs();
            const bool imuOn = imuSeenUs && clock.nowUs() - imuSeenUs < kImuGoneUs;
            if (imuOn != imuAnswered) {
                imuAnswered = imuOn;
                app.setImuPresent(imuOn);
                if (!imuOn)
                    LOG_WARNING(Imu, "No IMU data for %u s: AgOpenWeb no longer sees an IMU",
                                static_cast<unsigned>(kImuGoneUs / 1'000'000));
            }
            st.radioBytes = gnss.counters().radioBytes;
            st.nmeaOutSent = gnss.counters().nmeaOutSent;
            st.rtcmOutBytes = gnss.counters().rtcmOutBytes;
            st.nmeaCorrected = gnss.correctedActive();
            st.correctedFixes = gnss.counters().correctedFixes;
            st.correctedSent = gnss.counters().correctedSent;
            st.nmeaOutDropped = gnss.counters().nmeaOutDropped;
            for (int t = 0; t < 5; ++t)
                if (gnss.nmeaOutLastUs(t) && clock.nowUs() - gnss.nmeaOutLastUs(t) < 3'000'000)
                    std::memcpy(st.nmeaOutLast[t], gnss.nmeaOutLast(t), sizeof st.nmeaOutLast[t]);
            st.rtcmOutCount = gnss.rtcmOutMonitor().rtcmTypes(clock.nowUs(), st.rtcmOutTypes, aow::PortMonitor::kRtcmTypes);
            hub.status(st);
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
            st.loop = loopTiming.stats();  // up to the last tick: this one is recorded below
            st.loopSetup = loopSetup;
            shared.setStatus(st);

            if (ticks >= nextSummary) {
                nextSummary = ticks + summaryTicks;
                logSummary(app, &gnss, autosteer, &udp);
                const aow::LoopStats& ls = loopTiming.stats();
                LOG_INFO(System, "Loop: late max %u us (ever %u), work max %u us (ever %u), inputs max %u us (ever %u), missed %u, >1 ms late %u",
                         ls.lateMaxUs, ls.lateMaxEverUs, ls.workMaxUs, ls.workMaxEverUs, ls.inputsMaxUs,
                         ls.inputsMaxEverUs, ls.missed, ls.lateOver1ms);
            }
            loopTiming.record(ticks, tickBeginUs, inputsBeginUs, inputsEndUs, clock.nowUs());
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
    close(sigFd);
    return fail(code);
}
