// Autosteer: arming, kickouts, timeouts and the motor command. Ported from the firmware's
// AutosteerProcessor, KickoutMonitor and ADProcessor (WAS, current, switches).
//
// Safety model (docs/PLAN.md section 4): the motor may run only while shouldSteer() holds:
//   armed, guidance on, speed above minimum, a PGN 254 in the last 2 s, and no kickout
//   cooldown. Every path that drops one of those is unit-tested (tests/test_steer.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

#include "aow/hal.h"
#include "aow/settings.h"
#include "aow/steer_pgn.h"

namespace aow::steer {

// Firmware timings.
constexpr uint32_t kSteerTimeoutMs = 2000;        // no PGN 254 for this long: motor off
constexpr uint32_t kKickoutCooldownMs = 2000;     // motor stays off this long after a kickout
constexpr uint32_t kDebounceMs = 50;
constexpr uint32_t kGuidanceOffDisarmMs = 300;    // guidance off this long (no switch): disarm
constexpr uint32_t kKickoutGraceMs = 5000;        // re-arming within this clears the kickout
constexpr uint32_t kGuidanceToggleMs = 1000;      // guidance off-on within this clears a kickout
constexpr uint32_t kCurrentSpikeFilterMs = 1000;  // current must stay high this long to kick out

// Current in the firmware's units: Teensy ADC counts above a 77-count baseline, full scale
// 1680 counts. Keeping the unit keeps the app's current threshold meaning the same.
constexpr float kCurrentCountsPerVolt = 4095.0f / 3.3f;  // TO CALIBRATE with the HAT's driver
constexpr float kCurrentBaselineCounts = 77.0f;
constexpr float kCurrentFullScaleCounts = 1680.0f;

// What the board reads each tick.
struct Inputs {
    uint16_t wasCounts = 2048;  // 12-bit, 2048 = centre (2.5 V on the HAT's ratiometric ADC)
    float currentVolts = 0;
    bool steerSwitch = false;   // raw contacts, true = closed
    bool workSwitch = false;
    bool remoteSwitch = false;
};

// The motor command. enable false means unpowered, whatever pwm says.
struct Output {
    bool enable = false;
    int16_t pwm = 0;  // -255..255, sign = direction (after motorDriveDirection)
};

enum class MotorState : uint8_t { Disabled, SoftStart, SoftAccel, Normal };
enum class Kickout : uint8_t { None, CurrentHigh };

class Debouncer {
public:
    // Returns true when the debounced state changes.
    bool update(bool raw, uint64_t nowMs);
    bool state() const { return debounced_; }

private:
    bool current_ = false, debounced_ = false;
    uint64_t changedAt_ = 0;
};

// (counts - 2048 - offset) / counts-per-degree, inverted if set. 0 if counts-per-degree is 0.
float wasAngle(uint16_t counts, const Settings& s);

class Autosteer {
public:
    explicit Autosteer(hal::Clock& clock, const Settings& s = Settings());

    // From the app, per frame.
    void onSteerData(const pgn::SteerCommand& c);
    // PGN 251 / 252. Each returns true if the settings changed (the caller saves them).
    bool onSteerConfig(const uint8_t* data, size_t len);
    bool onSteerSettings(const uint8_t* data, size_t len);

    // The 100 Hz control step.
    Output tick(const Inputs& in);

    // What PGN 253 reports.
    pgn::SteerStatus status() const;
    // PGN 250: motor current, 0..255 of full scale, when the current sensor is on; else 0.
    uint8_t turnSensorReading() const;

    const Settings& settings() const { return s_; }
    void setSettings(const Settings& s) { s_ = s; }

    // For status pages and tests.
    bool armed() const { return armed_; }
    bool guidanceActive() const { return guidance_; }
    MotorState motorState() const { return motor_; }
    Kickout kickout() const { return kickout_; }
    bool shouldSteer() const;
    float wasAngleDeg() const { return wasAngle_; }
    float actualAngleDeg() const { return actualAngle_; }
    float targetAngleDeg() const { return cmd_.targetAngleDeg; }
    float speedKmh() const { return cmd_.speedKmh; }
    uint16_t currentCounts() const { return currentCounts_; }
    bool workSwitch() const;
    bool steerSwitch() const { return steerSw_.state(); }

private:
    uint64_t nowMs() const { return clock_.nowUs() / 1000; }
    void updateArming(uint64_t now);
    void updateCurrent(float volts);
    void updateKickout(uint64_t now);
    bool checkCurrentKickout(uint64_t now);
    Output updateMotor(uint64_t now);
    void emergencyStop(uint64_t now);
    void disarm(const char* why);

    hal::Clock& clock_;
    Settings s_;

    // From PGN 254.
    pgn::SteerCommand cmd_;
    bool haveCommand_ = false;
    uint64_t commandAtMs_ = 0;
    bool guidance_ = false;
    bool firstCommand_ = true;
    bool guidanceRose_ = false, guidanceFell_ = false;  // since the last tick

    // Switches.
    Debouncer steerSw_, workSw_, remoteSw_;
    bool lastButtonPressed_ = false;
    bool lastSwitchOn_ = false;

    // Arming (the firmware's steerState, inverted: armed_ == steerState 0).
    bool armed_ = false;
    uint64_t guidanceOffSinceMs_ = 0;
    bool guidanceOffTiming_ = false;

    // Kickout.
    Kickout kickout_ = Kickout::None;
    uint64_t kickoutAtMs_ = 0;           // for the motor cooldown (the firmware's kickoutTime)
    uint64_t currentHighSinceMs_ = 0;
    bool kickoutGracePending_ = false;
    uint64_t kickoutGraceStartMs_ = 0;
    bool waitingForGuidanceOn_ = false;
    uint64_t guidanceOffAtMs_ = 0;

    // Sensors.
    float wasAngle_ = 0, actualAngle_ = 0;
    float currentWindow_[5] = {};  // 50 ms at 100 Hz, as the firmware's 50 samples at 1 kHz
    size_t currentIndex_ = 0;
    uint16_t currentCounts_ = 0;

    // Motor.
    MotorState motor_ = MotorState::Disabled;
    uint64_t rampStartMs_ = 0;
    int16_t drive_ = 0;   // before motorDriveDirection
    int16_t output_ = 0;  // after it
};

}  // namespace aow::steer
