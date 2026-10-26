// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
// Ported from Firmware_Teensy_AiO-New-Dawn, copyright 2025 by the AOG Group.
//
// Motor drivers as the autosteer sees them: the firmware's MotorDriverInterface plus the
// TractorCANDriver queries AutosteerProcessor made.
//
//   PWM (the HAT's PWM + DIR + EN, or the simulator)   daemon/board.h
//   Tractor CAN (brands, JSON engine, Keya on CAN)      TractorCanMotor, wrapping the ported driver
//   Keya over RS-232                                    KeyaSerialMotor, ported from KeyaSerialDriver
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "aow/can/TractorCANDriver.h"
#include "aow/hal.h"
#include "aow/settings.h"

namespace aow::motor {

class Motor {
public:
    virtual ~Motor() = default;
    virtual const char* name() const = 0;
    // Every control tick: enable false must leave the motor unpowered.
    virtual void command(bool enable, int16_t pwm) = 0;
    // 50 Hz, as the firmware's motor task.
    virtual void process() {}
    // MotorStatus.hasError: the autosteer stops on it while steering.
    virtual bool hasError() const { return false; }
    // Keya and tractor CAN report their own problems; the current kickout is off for them.
    virtual bool ownsKickout() const { return false; }

    // Tractor CAN only.
    virtual bool isTractorCan() const { return false; }
    virtual bool ready() const { return true; }            // valve ready / Keya heartbeat
    virtual const char* notReadyMessage() const { return ""; }
    virtual bool takeEngageEvent(std::string& label) { (void)label; return false; }  // a CAN steer button press
    virtual bool canKickout() const { return false; }
    virtual void clearCanKickout() {}
    virtual bool canWheelAngle(float& deg) const { (void)deg; return false; }
};

class TractorCanMotor : public Motor {
public:
    // cfg: the CAN settings; customJson: the uploaded brand file, or "" for the built-in code.
    TractorCanMotor(const CANSteerConfig& cfg, std::string customJson);

    const char* name() const override { return driver_.getTypeName(); }
    void command(bool enable, int16_t pwm) override;
    void process() override { driver_.process(); }
    bool hasError() const override { return driver_.getStatus().hasError; }
    bool ownsKickout() const override { return true; }

    bool isTractorCan() const override { return true; }
    bool ready() const override;
    const char* notReadyMessage() const override;
    bool takeEngageEvent(std::string& label) override;
    bool canKickout() const override { return driver_.isKickoutDetected(); }
    void clearCanKickout() override { driver_.resetKickoutDetected(); }
    bool canWheelAngle(float& deg) const override;

    TractorCANDriver& driver() { return driver_; }

private:
    TractorCANDriver driver_;
    // The firmware's AutosteerProcessor kept these as statics: the legacy brand buttons.
    bool lastMassey_ = false, lastFendt_ = false, lastCaseIh_ = false, lastCatMt_ = false;
    bool lastClaas_ = false, lastJcb_ = false, lastLindner_ = false;
};

class KeyaSerialMotor : public Motor {
public:
    KeyaSerialMotor(hal::SerialPort& port, hal::Clock& clock);

    // MotorDriverManager::probeKeyaSerial: send E2 00 00 E2, expect E2 back within 100 ms.
    // Blocks for up to 100 ms; call once at startup.
    static bool probe(hal::SerialPort& port, hal::Clock& clock);

    const char* name() const override { return "Keya Serial"; }
    void command(bool enable, int16_t pwm) override;
    void process() override;
    bool hasError() const override;
    bool ownsKickout() const override { return true; }

    int8_t actualRpm() const { return actualRpm_; }
    float currentAmps() const { return motorCurrent_ * 0.1f; }

private:
    uint32_t millis() const { return static_cast<uint32_t>(clock_.nowUs() / 1000); }
    void sendCommand();
    void checkResponse();
    bool checkMotorSlip();

    hal::SerialPort& port_;
    hal::Clock& clock_;
    bool enabled_ = false;
    int16_t targetPwm_ = 0;
    uint8_t response_[16] = {};
    uint8_t responseIndex_ = 0;
    uint32_t lastCommandTime_ = 0, lastResponseTime_ = 0;
    bool hasValidResponse_ = false;
    int8_t actualRpm_ = 0;
    int8_t motorCurrent_ = 0;  // 0.1 A
    uint16_t motorErrorCode_ = 0;
    bool slipDetected_ = false;
    uint32_t slipStartTime_ = 0;
};

// The CAN part of the settings, as the firmware's CANSteerConfig.
CANSteerConfig canSteerConfig(const Settings& s);

// The firmware's getTractorValveMessage.
const char* valveNotReadyMessage(uint8_t brand);

}  // namespace aow::motor
