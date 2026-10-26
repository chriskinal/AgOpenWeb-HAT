// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
// Ported from Firmware_Teensy_AiO-New-Dawn, copyright 2025 by the AOG Group.

#include "aow/motor.h"

#include <cstdlib>

#include "aow/log.h"

namespace aow::motor {

const char* valveNotReadyMessage(uint8_t brand) {
    switch (static_cast<TractorBrand>(brand)) {
    case TractorBrand::VALTRA_MASSEY: return "Valtra/MF steering valve not ready! Check CAN connection.";
    case TractorBrand::FENDT:
    case TractorBrand::FENDT_ONE: return "Fendt steering valve not ready! Check CAN connection.";
    case TractorBrand::CASEIH_NH: return "Case IH/NH steering valve not ready! Check CAN connection.";
    case TractorBrand::CLAAS: return "CLAAS steering valve not ready! Check CAN connection.";
    case TractorBrand::JCB: return "JCB steering valve not ready! Check CAN connection.";
    case TractorBrand::CAT_MT: return "CAT MT steering valve not ready! Check CAN connection.";
    case TractorBrand::LINDNER: return "Lindner steering valve not ready! Check CAN connection.";
    case TractorBrand::GENERIC: return "Keya motor not responding! Check CAN connection.";
    default: return "Tractor steering not ready! Check CAN connection.";
    }
}

CANSteerConfig canSteerConfig(const Settings& s) {
    CANSteerConfig c;
    c.brand = s.can.brand;
    c.can1Speed = s.can.bus[0].speed; c.can1Function = s.can.bus[0].function; c.can1Name = s.can.bus[0].name;
    c.can2Speed = s.can.bus[1].speed; c.can2Function = s.can.bus[1].function; c.can2Name = s.can.bus[1].name;
    c.can3Speed = s.can.bus[2].speed; c.can3Function = s.can.bus[2].function; c.can3Name = s.can.bus[2].name;
    c.moduleID = s.can.moduleId;
    return c;
}

// --- Tractor CAN

TractorCanMotor::TractorCanMotor(const CANSteerConfig& cfg, std::string customJson) {
    driver_.configure(cfg, std::move(customJson));
    driver_.init();
}

void TractorCanMotor::command(bool enable, int16_t pwm) {
    // AutosteerProcessor: setPWM then enable while steering; enable(false), setPWM(0) when not.
    if (enable) {
        driver_.setPWM(pwm);
        driver_.enable(true);
    } else {
        driver_.enable(false);
        driver_.setPWM(0);
    }
}

bool TractorCanMotor::ready() const {
    return driver_.getCurrentBrand() == TractorBrand::GENERIC ? driver_.isHeartbeatValid() : driver_.isValveReady();
}

const char* TractorCanMotor::notReadyMessage() const {
    return valveNotReadyMessage(static_cast<uint8_t>(driver_.getCurrentBrand()));
}

bool TractorCanMotor::takeEngageEvent(std::string& label) {
    if (driver_.checkEngageEvent()) {
        label = driver_.getEngageLabel();
        return true;
    }
    // The legacy brand code's buttons: Massey and Fendt on release, the rest on press.
    struct Edge { bool now; bool& last; bool onRelease; const char* name; };
    Edge edges[] = {
        {driver_.isEngageButtonPressed(), lastMassey_, true, "Massey K_Bus button"},
        {driver_.isFendtButtonPressed(), lastFendt_, true, "Fendt armrest button"},
        {driver_.isCaseIHEngaged(), lastCaseIh_, false, "Case IH engage"},
        {driver_.isCATMTEngaged(), lastCatMt_, false, "CAT MT engage"},
        {driver_.isClaasEngaged(), lastClaas_, false, "CLAAS engage"},
        {driver_.isJcbEngaged(), lastJcb_, false, "JCB engage"},
        {driver_.isLindnerEngaged(), lastLindner_, false, "Lindner engage"},
    };
    bool fired = false;
    for (Edge& e : edges) {
        const bool edge = e.onRelease ? (!e.now && e.last) : (e.now && !e.last);
        if (edge && !fired) {
            label = e.name;
            fired = true;
        }
        e.last = e.now;
    }
    return fired;
}

bool TractorCanMotor::canWheelAngle(float& deg) const {
    if (!driver_.providesCANWheelAngle() || !driver_.isValveDataReceived()) return false;
    deg = static_cast<float>(driver_.getActualCurve()) / driver_.getCurveScale();
    return true;
}

// --- Keya serial (KeyaSerialDriver)

KeyaSerialMotor::KeyaSerialMotor(hal::SerialPort& port, hal::Clock& clock) : port_(port), clock_(clock) {
    uint8_t drain[64];
    while (port_.read(drain, sizeof drain) > 0) {}
}

bool KeyaSerialMotor::probe(hal::SerialPort& port, hal::Clock& clock) {
    uint8_t drain[64];
    while (port.read(drain, sizeof drain) > 0) {}
    const uint8_t query[4] = {0xE2, 0x00, 0x00, 0xE2};
    port.write(query, 4);
    const uint64_t start = clock.nowUs();
    uint8_t response[5];
    size_t n = 0;
    while (clock.nowUs() - start < 100'000 && n < 5) {
        const int got = port.read(response + n, 5 - n);
        if (got > 0) n += static_cast<size_t>(got);
    }
    return n >= 4 && response[0] == 0xE2;
}

void KeyaSerialMotor::command(bool enable, int16_t pwm) {
    if (enable != enabled_ && !enable) {
        slipDetected_ = false;
        slipStartTime_ = 0;
    }
    enabled_ = enable;
    targetPwm_ = enable ? static_cast<int16_t>(pwm < -255 ? -255 : pwm > 255 ? 255 : pwm) : 0;
}

void KeyaSerialMotor::process() {
    checkResponse();
    if (millis() - lastCommandTime_ >= 20) sendCommand();
    if (enabled_) slipDetected_ = checkMotorSlip();
}

void KeyaSerialMotor::sendCommand() {
    uint8_t cmd[4];
    if (enabled_ && targetPwm_ != 0) {
        const int16_t speed = static_cast<int16_t>(targetPwm_ * 1000 / 255);
        cmd[0] = 0xAD;
        cmd[1] = static_cast<uint8_t>((speed >> 8) & 0xFF);
        cmd[2] = static_cast<uint8_t>(speed & 0xFF);
    } else {
        cmd[0] = 0xAC;
        cmd[1] = 0x00;
        cmd[2] = 0x00;
    }
    cmd[3] = static_cast<uint8_t>((cmd[0] + cmd[1] + cmd[2]) & 0xFF);
    uint8_t drain[64];
    while (port_.read(drain, sizeof drain) > 0) {}  // as the firmware: drop anything half-read
    port_.write(cmd, 4);
    lastCommandTime_ = millis();
    responseIndex_ = 0;
}

void KeyaSerialMotor::checkResponse() {
    while (responseIndex_ < sizeof response_) {
        const int got = port_.read(response_ + responseIndex_, 1);
        if (got <= 0) break;
        ++responseIndex_;
    }
    if (responseIndex_ >= 15) {
        if (response_[0] == 0xAC && response_[5] == 0xAD && response_[10] == 0xAE) {
            hasValidResponse_ = true;
            lastResponseTime_ = millis();
            actualRpm_ = static_cast<int8_t>(response_[6]);
            motorCurrent_ = static_cast<int8_t>(response_[7]);
            motorErrorCode_ = static_cast<uint16_t>(response_[11] << 8 | response_[12]);
        }
        responseIndex_ = 0;
    }
}

bool KeyaSerialMotor::checkMotorSlip() {
    if (!enabled_ || !hasValidResponse_ || targetPwm_ == 0) {
        slipStartTime_ = 0;
        return false;
    }
    const int commanded = targetPwm_ * 100 / 255;
    const int error = std::abs(commanded - actualRpm_);
    if (error > std::abs(commanded) * 0.3f || error > 20) {
        if (slipStartTime_ == 0) slipStartTime_ = millis();
        if (millis() - slipStartTime_ > 200) {
            LOG_WARNING(Autosteer, "Keya motor slip: commanded %d RPM, actual %d RPM", commanded, actualRpm_);
            return true;
        }
    } else {
        slipStartTime_ = 0;
    }
    return false;
}

bool KeyaSerialMotor::hasError() const {
    return !hasValidResponse_ || millis() - lastResponseTime_ > 1000 ||
           (motorErrorCode_ != 0 && motorErrorCode_ != 0x0001) || slipDetected_;
}

}  // namespace aow::motor
