// A snapshot of the daemon's state, published by the control loop for the web pages and the
// telemetry stream. Plain data: copied under a lock, never shared by reference.
#pragma once

#include <cstdint>

#include "aow/port_monitor.h"

namespace aow {

struct Status {
    uint64_t uptimeMs = 0;

    // Steering (the fields the firmware's telemetry packet carries).
    float wasAngle = 0;        // deg
    float targetAngle = 0;     // deg, from PGN 254
    float currentAmps = 0;
    float speedKph = 0;        // from PGN 254
    float heading = 0;         // deg, GNSS
    bool autosteerActive = false;
    bool steerSwitch = false;
    bool workSwitch = false;
    bool haveAppLink = false;  // PGN 254 seen in the last 2 s

    // GNSS.
    uint8_t fixQuality = 0;
    uint8_t satellites = 0;
    uint32_t navSent = 0;

    // External IMU (AOW HAT: not in the firmware's status).
    const char* imuType = "None";
    char imuPort[96] = {};         // the device the IMU in use is read from; empty: none
    char gnssPort[96] = {};        // the device of the receiver in use; empty: none
    uint32_t radioBytes = 0, nmeaOutSent = 0, rtcmOutBytes = 0;  // serial devices at work
    // NMEA out's position: the app's (PGN 100) or the receiver's; PGN 100s in, sentences sent
    // with its position, sentences dropped for want of room on the port.
    bool nmeaCorrected = false;
    uint32_t correctedFixes = 0, correctedSent = 0, nmeaOutDropped = 0;
    // Going Out Now: the last sentence NMEA out wrote of each type in the last 3 s (GGA, VTG,
    // RMC, ZDA, HDT; empty: none), and the RTCM message numbers RTCM out sent lately.
    char nmeaOutLast[5][121] = {};
    int rtcmOutCount = 0;
    uint16_t rtcmOutTypes[PortMonitor::kRtcmTypes] = {};

    // Every serial port and USB serial device, and what is coming in on it (serial_hub.h, the
    // Serial Communications page).
    static constexpr int kSerialChannels = 10;
    struct Serial {
        char name[96] = {};          // "Onboard", "Port 1", "Port 2", a USB /dev/serial/by-id name
        const char* place = "";      // "onboard", "port1", "port2", "usb", "fixed"
        const char* kind = "unknown";  // what it was found to be: "gnss", "radio", "imu", "unknown"
        bool open = false, inUse = false, nmeaOut = false, rtcmOut = false, autoBaud = false;
        unsigned baud = 0;
        uint32_t bytesIn = 0;
        uint32_t msSinceIn = UINT32_MAX;  // UINT32_MAX: nothing yet
        int lineCount = 0;
        char lines[PortMonitor::kLines][PortMonitor::kLineChars + 1] = {};
        int rtcmCount = 0;
        uint16_t rtcm[PortMonitor::kRtcmTypes] = {};
    } serial[kSerialChannels];
    int serialCount = 0;
    bool imuValid = false;
    float imuHeading = 0, imuRoll = 0, imuPitch = 0, imuYawRate = 0;  // deg, deg/s

    // Times each CAN channel's interface was restarted after it stopped sending (can_watch.h).
    uint32_t canRestarts[3] = {};

    // The four status LEDs (indicators::Color as numbers: 0 off, 1 red, 2 amber, 3 green, 4 blue).
    uint8_t leds[4] = {};
};

// The ISOBUS stack's state (docs/PLAN.md section 8), published by its own thread.
struct IsobusStatus {
    enum class State : uint8_t {
        NotBuilt,   // the daemon was built without the stack
        Disabled,   // switched off in the settings
        Starting,   // channel open, address claim in progress
        Claimed,    // on the bus with an address
        Failed,     // the channel did not open, or no address could be claimed
    };
    State state = State::Disabled;
    uint8_t channel = 0;       // 1..3, the HAT's CAN1..CAN3
    uint8_t address = 0xFE;    // J1939 source address; 0xFE is "none"
    // A frame of ours has been acknowledged, so something else is on the bus. A claim on an
    // empty or unplugged bus still "succeeds": nothing objected.
    bool busActive = false;

    // The task controller: the first implement with sections (one boom is what the app drives).
    uint8_t clients = 0;           // control functions connected to the task controller
    uint8_t sections = 0;          // the implement's section count; 0 = no implement
    bool sectionControl = false;   // setpoints are being sent (auto mode)
    uint64_t setpoint = 0;         // section bits asked for, section 1 in bit 0
    uint64_t actual = 0;           // section bits the implement reports on
    float widthM = 0;              // working width from the implement's description; 0 = unknown
    char implement[40] = {};       // its designator

    // The status screen on the tractor's terminal (upstream's object pool).
    enum class Terminal : uint8_t {
        Off,        // switched off in the settings
        Waiting,    // no terminal on the bus, or it has not taken the screen yet
        Connected,  // the screen is on the terminal
    };
    Terminal terminal = Terminal::Off;

    // The tractor ECU role: ground speed on the bus, at address 240.
    enum class Tecu : uint8_t {
        Off,       // switched off in the settings
        Waiting,   // listening for a tractor ECU already on the bus, or claiming 240
        Active,    // we are the tractor ECU
        Tractor,   // the bus has its own tractor ECU: ours stays off
        Failed,    // address 240 could not be claimed
    };
    Tecu tecu = Tecu::Off;
    int32_t speedMmS = 0;          // what the tractor ECU is sending
    uint64_t name = 0;         // the 64-bit NAME we claim with
    char interface[16] = {};
};
inline const char* isobusStateName(IsobusStatus::State s) {
    switch (s) {
    case IsobusStatus::State::NotBuilt: return "not built";
    case IsobusStatus::State::Disabled: return "disabled";
    case IsobusStatus::State::Starting: return "starting";
    case IsobusStatus::State::Claimed: return "claimed";
    case IsobusStatus::State::Failed: return "failed";
    }
    return "";
}
inline const char* isobusTerminalName(IsobusStatus::Terminal t) {
    switch (t) {
    case IsobusStatus::Terminal::Off: return "off";
    case IsobusStatus::Terminal::Waiting: return "waiting";
    case IsobusStatus::Terminal::Connected: return "connected";
    }
    return "";
}

inline const char* isobusTecuName(IsobusStatus::Tecu t) {
    switch (t) {
    case IsobusStatus::Tecu::Off: return "off";
    case IsobusStatus::Tecu::Waiting: return "waiting";
    case IsobusStatus::Tecu::Active: return "active";
    case IsobusStatus::Tecu::Tractor: return "tractor has one";
    case IsobusStatus::Tecu::Failed: return "failed";
    }
    return "";
}

}  // namespace aow
