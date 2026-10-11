// A snapshot of the daemon's state, published by the control loop for the web pages and the
// telemetry stream. Plain data: copied under a lock, never shared by reference.
#pragma once

#include <cstdint>

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

}  // namespace aow
