#include "aow/steer.h"

#include <cmath>
#include <cstdlib>

#include "aow/log.h"

namespace aow::steer {

namespace {

constexpr uint8_t kDirectionChangeThreshold = 20;  // PWM beyond minPWM that counts as a reversal
constexpr float kAccelThresholdRatio = 0.33f;

int16_t clampPwm(int v, int limit) { return static_cast<int16_t>(v > limit ? limit : v < -limit ? -limit : v); }

}  // namespace

// --- Debouncer (the firmware's debounceSwitch)

bool Debouncer::update(bool raw, uint64_t nowMs) {
    if (raw != current_) {
        current_ = raw;
        changedAt_ = nowMs;
    } else if (current_ != debounced_ && nowMs - changedAt_ >= kDebounceMs) {
        debounced_ = current_;
        return true;
    }
    return false;
}

float wasAngle(uint16_t counts, const Settings& s) {
    if (s.gains.steerSensorCounts == 0) return 0;
    const float angle = (static_cast<float>(counts) - 2048.0f - s.gains.wasOffset) / s.gains.steerSensorCounts;
    return s.steer.invertWas ? -angle : angle;
}

// --- Autosteer

Autosteer::Autosteer(hal::Clock& clock, const Settings& s) : clock_(clock), s_(s) {}

bool Autosteer::workSwitch() const { return workSw_.state() != s_.misc.invertWorkSwitch; }

void Autosteer::onSteerData(const pgn::SteerCommand& c) {
    const bool g = c.guidanceActive();
    if (firstCommand_) {
        firstCommand_ = false;
    } else if (g != guidance_) {
        // Edges are kept until the next tick: at 100 Hz two frames can land in one tick.
        (g ? guidanceRose_ : guidanceFell_) = true;
    }
    guidance_ = g;
    cmd_ = c;
    haveCommand_ = true;
    commandAtMs_ = nowMs();
}

bool Autosteer::onSteerConfig(const uint8_t* d, size_t len) {
    if (len < 4) {
        LOG_ERROR(Autosteer, "PGN 251 too short: %zu bytes", len);
        return false;
    }
    if (kickout_ != Kickout::None) {
        kickout_ = Kickout::None;
        currentHighSinceMs_ = 0;
        LOG_INFO(Autosteer, "KICKOUT: Cleared via steer config update");
    }
    Settings n = s_;
    const uint8_t sett0 = d[0], sett1 = d[3];
    n.steer.invertWas = sett0 & 0x01;
    n.steer.motorDriveDirection = sett0 & 0x04;
    n.steer.cytronDriver = sett0 & 0x10;
    n.steer.steerSwitch = sett0 & 0x20;
    n.steer.steerButton = sett0 & 0x40;
    n.steer.minSpeed = d[2];
    n.steer.currentSensor = sett1 & 0x04;
    if (n.steer.currentSensor) n.steer.currentThreshold = d[1];
    n.steer.motorDriverConfig = d[3];
    if ((sett1 & 0x01) || (n.steer.motorDriverConfig & 0x01)) n.steer.cytronDriver = false;  // Danfoss

    LOG_INFO(Autosteer, "Steer config: WAS=%s Motor=%s MinSpeed=%u Steer=%s Current=%s (threshold %u)",
             n.steer.invertWas ? "Inv" : "Norm", n.steer.motorDriveDirection ? "Rev" : "Norm", n.steer.minSpeed,
             n.steer.steerButton ? "Button" : n.steer.steerSwitch ? "Switch" : "None",
             n.steer.currentSensor ? "Yes" : "No", n.steer.currentThreshold);
    const bool changed = toJson(n) != toJson(s_);
    s_ = n;
    return changed;
}

bool Autosteer::onSteerSettings(const uint8_t* d, size_t len) {
    if (len < 8) {
        LOG_ERROR(Autosteer, "PGN 252 too short: %zu bytes", len);
        return false;
    }
    Settings n = s_;
    n.gains.kp = d[0];
    n.gains.highPwm = d[1];
    n.gains.minPwm = d[3];  // byte 2 (low PWM) is unused, as in the firmware
    n.gains.steerSensorCounts = d[4];
    n.gains.wasOffset = static_cast<int16_t>(d[5] | d[6] << 8);
    n.gains.ackermanFix = static_cast<float>(d[7]) * 0.01f;
    LOG_INFO(Autosteer, "Steer settings: Kp=%u PWM min=%u high=%u WAS offset=%d counts=%u Ackerman=%.2f",
             d[0], n.gains.minPwm, n.gains.highPwm, n.gains.wasOffset, n.gains.steerSensorCounts,
             static_cast<double>(n.gains.ackermanFix));
    const bool changed = toJson(n) != toJson(s_);
    s_ = n;
    return changed;
}

void Autosteer::disarm(const char* why) {
    if (armed_) LOG_INFO(Autosteer, "Autosteer DISARMED - %s", why);
    armed_ = false;
}

void Autosteer::emergencyStop(uint64_t now) {
    LOG_WARNING(Autosteer, "EMERGENCY STOP");
    motor_ = MotorState::Disabled;
    drive_ = output_ = 0;
    armed_ = false;
    kickoutAtMs_ = now;
}

void Autosteer::updateArming(uint64_t now) {
    // Physical button (toggles) or switch (follows).
    if (s_.steer.steerButton) {
        const bool pressed = steerSw_.state();
        if (pressed && !lastButtonPressed_) {
            armed_ = !armed_;
            LOG_INFO(Autosteer, "Autosteer %s via button", armed_ ? "ARMED" : "DISARMED");
        }
        lastButtonPressed_ = pressed;
    } else if (s_.steer.steerSwitch) {
        const bool on = steerSw_.state();
        if (on != lastSwitchOn_) {
            armed_ = on;
            LOG_INFO(Autosteer, "Autosteer %s via switch", armed_ ? "ARMED" : "DISARMED");
            lastSwitchOn_ = on;
        }
    }

    // Guidance switched on in the app: arm, and clear a kickout.
    if (guidanceRose_ && guidance_) {
        armed_ = true;
        LOG_INFO(Autosteer, "Autosteer ARMED via AgOpenGPS (Guidance ON)");
        if (kickout_ != Kickout::None) {
            kickout_ = Kickout::None;
            currentHighSinceMs_ = 0;
            LOG_INFO(Autosteer, "KICKOUT: Cleared via AgOpenGPS (Guidance ON)");
        }
    }

    // Guidance off for 300 ms with no switch holding it: disarm.
    const bool switchHolds = s_.steer.steerSwitch && steerSw_.state();
    if (armed_ && !guidance_ && !switchHolds && !s_.steer.steerButton) {
        if (!guidanceOffTiming_) {
            guidanceOffTiming_ = true;
            guidanceOffSinceMs_ = now;
        } else if (now - guidanceOffSinceMs_ > kGuidanceOffDisarmMs) {
            disarm("guidance inactive");
            guidanceOffTiming_ = false;
        }
    } else {
        guidanceOffTiming_ = false;
    }
}

void Autosteer::updateCurrent(float volts) {
    float counts = volts * kCurrentCountsPerVolt - kCurrentBaselineCounts;
    if (counts < 0) counts = 0;
    currentWindow_[currentIndex_] = counts;
    currentIndex_ = (currentIndex_ + 1) % 5;
    float sum = 0;
    for (float c : currentWindow_) sum += c;
    currentCounts_ = static_cast<uint16_t>(sum / 5);
}

// The firmware's checkCurrentKickout: over threshold + 10% for a full second. It re-arms its
// timer when it fires, so a kickout is reported once and then reads as cleared.
bool Autosteer::checkCurrentKickout(uint64_t now) {
    const uint16_t threshold = static_cast<uint16_t>(s_.steer.currentThreshold * kCurrentFullScaleCounts / 255);
    const uint16_t withHysteresis = threshold + threshold / 10;
    if (currentCounts_ <= withHysteresis) {
        currentHighSinceMs_ = 0;
        return false;
    }
    if (currentHighSinceMs_ == 0) {
        currentHighSinceMs_ = now;
        return false;
    }
    if (now - currentHighSinceMs_ >= kCurrentSpikeFilterMs) {
        currentHighSinceMs_ = 0;
        return true;
    }
    return false;
}

void Autosteer::updateKickout(uint64_t now) {
    // KickoutMonitor::process: detect, or clear once the condition has gone.
    if (kickout_ == Kickout::None) {
        if (s_.steer.currentSensor && checkCurrentKickout(now)) {
            kickout_ = Kickout::CurrentHigh;
            LOG_WARNING(Autosteer, "KICKOUT: Current High (%u counts)", currentCounts_);
        }
    } else if (!(kickout_ == Kickout::CurrentHigh && s_.steer.currentSensor && checkCurrentKickout(now))) {
        kickout_ = Kickout::None;
        currentHighSinceMs_ = 0;
    }

    // AutosteerProcessor's handling.
    if (kickout_ != Kickout::None && armed_) {
        emergencyStop(now);
        LOG_WARNING(Autosteer, "KICKOUT: steering disarmed");
        kickoutGracePending_ = true;
        kickoutGraceStartMs_ = now;
    }
    if (kickout_ != Kickout::None && armed_ && kickoutGracePending_ && now - kickoutGraceStartMs_ < kKickoutGraceMs) {
        kickout_ = Kickout::None;
        kickoutGracePending_ = false;
        LOG_INFO(Autosteer, "KICKOUT: Cleared via button/switch during grace period");
    }
    if (kickout_ != Kickout::None && !armed_) {
        if (guidanceFell_) {
            guidanceOffAtMs_ = now;
            waitingForGuidanceOn_ = true;
        } else if (guidanceRose_ && guidance_ && waitingForGuidanceOn_ && now - guidanceOffAtMs_ < kGuidanceToggleMs) {
            waitingForGuidanceOn_ = false;
            kickout_ = Kickout::None;
            armed_ = true;
            LOG_INFO(Autosteer, "KICKOUT: Cleared via guidance toggle");
        }
    }
}

bool Autosteer::shouldSteer() const {
    const uint64_t now = nowMs();
    if (kickoutAtMs_ && now - kickoutAtMs_ < kKickoutCooldownMs) return false;
    if (!haveCommand_ || now - commandAtMs_ > kSteerTimeoutMs) return false;
    return guidance_ && armed_ && cmd_.speedKmh > s_.steer.minSpeed / 10.0f;
}

Output Autosteer::updateMotor(uint64_t now) {
    const bool active = shouldSteer();
    if (active && motor_ == MotorState::Disabled) {
        motor_ = MotorState::SoftStart;
        rampStartMs_ = now;
        LOG_INFO(Autosteer, "Motor STARTING - soft-start sequence (%ums)", s_.steer.softStartDurationMs);
    } else if (!active && motor_ != MotorState::Disabled) {
        motor_ = MotorState::Disabled;
        drive_ = output_ = 0;
        const char* why = !haveCommand_ || now - commandAtMs_ > kSteerTimeoutMs ? "communication timeout"
                          : !guidance_                                         ? "guidance inactive"
                          : !armed_                                            ? "steer switch off"
                          : cmd_.speedKmh <= s_.steer.minSpeed / 10.0f         ? "speed too low"
                                                                               : "kickout cooldown";
        LOG_INFO(Autosteer, "Motor disabled - %s", why);
        return {};
    }
    if (!active) return {};

    const int kp = static_cast<int>(s_.gains.kp);
    const int highPwm = s_.gains.highPwm, minPwm = s_.gains.minPwm;
    if (highPwm == 0) {
        LOG_ERROR(Autosteer, "Invalid PWM configuration (high PWM 0)");
        drive_ = output_ = 0;
        return {true, 0};
    }

    const float error = actualAngle_ - cmd_.targetAngleDeg;
    const float p = kp * error;  // the firmware truncates to int16; clamp first so the cast is defined
    int drive = static_cast<int>(p > 32767.0f ? 32767.0f : p < -32767.0f ? -32767.0f : p);
    if (drive < 0) drive -= minPwm;
    else if (drive > 0) drive += minPwm;
    drive = clampPwm(drive, highPwm);

    // Hard acceleration or a reversal: ramp again. Compared before motorDriveDirection (the
    // firmware compared the inverted previous output, which misfires with the motor reversed).
    const int accelThreshold = static_cast<int>((highPwm - minPwm) * kAccelThresholdRatio);
    if (motor_ == MotorState::Normal && std::abs(drive) > highPwm - accelThreshold &&
        std::abs(drive_) < minPwm + accelThreshold) {
        motor_ = MotorState::SoftAccel;
        rampStartMs_ = now;
    }
    if (motor_ == MotorState::Normal && ((drive_ > 0 && drive < 0) || (drive_ < 0 && drive > 0)) &&
        std::abs(drive) > minPwm + kDirectionChangeThreshold) {
        motor_ = MotorState::SoftStart;
        rampStartMs_ = now;
    }

    if (motor_ == MotorState::SoftStart || motor_ == MotorState::SoftAccel) {
        const uint32_t duration = motor_ == MotorState::SoftStart ? s_.steer.softStartDurationMs
                                                                  : s_.steer.softStartDurationMs / 2u;
        const uint64_t elapsed = now - rampStartMs_;
        if (elapsed >= duration) {
            motor_ = MotorState::Normal;
        } else {
            const int original = drive;
            drive = static_cast<int>(static_cast<float>(drive) * (static_cast<float>(elapsed) / duration));
            if (original != 0 && std::abs(drive) < minPwm) drive = original > 0 ? minPwm : -minPwm;
        }
    }

    drive_ = static_cast<int16_t>(drive);
    output_ = s_.steer.motorDriveDirection ? static_cast<int16_t>(-drive) : drive_;
    return {true, output_};
}

Output Autosteer::tick(const Inputs& in) {
    const uint64_t now = nowMs();

    steerSw_.update(in.steerSwitch, now);
    workSw_.update(in.workSwitch, now);
    remoteSw_.update(in.remoteSwitch, now);

    updateArming(now);

    updateCurrent(in.currentVolts);
    updateKickout(now);

    wasAngle_ = wasAngle(in.wasCounts, s_);
    actualAngle_ = wasAngle_ < 0 ? wasAngle_ * s_.gains.ackermanFix : wasAngle_;

    const Output out = updateMotor(now);
    guidanceRose_ = guidanceFell_ = false;
    return out;
}

pgn::SteerStatus Autosteer::status() const {
    pgn::SteerStatus st;
    st.actualAngleDeg = actualAngle_;
    st.workSwitchOn = workSwitch();
    st.steerArmed = armed_;
    st.remoteOrKickout = remoteSw_.state();
    st.pwmDisplay = static_cast<uint8_t>(std::abs(output_));
    return st;
}

uint8_t Autosteer::turnSensorReading() const {
    if (!s_.steer.currentSensor) return 0;
    const float scaled = currentCounts_ * 255.0f / kCurrentFullScaleCounts;
    return static_cast<uint8_t>(scaled > 255 ? 255 : scaled);
}

}  // namespace aow::steer
