// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
// Ported from Firmware_Teensy_AiO-New-Dawn, copyright 2025 by the AOG Group.
//
// UM981/UM982 receiver setup over the GNSS serial port, ported from the firmware's
// UM98xManager: read the receiver's CONFIG, MODE and log list, or write them and SAVECONFIG.
//
// Blocking (seconds): run it off the control thread, with the GNSS stream paused.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "aow/hal.h"

namespace aow {

class Um98x {
public:
    struct Config {
        std::string configCommands;   // one CONFIG command per line
        std::string modeSettings;     // the MODE command
        std::string messageSettings;  // one log command per line
    };

    // sleepMs lets the caller block for real (daemon) or advance a fake clock (tests).
    Um98x(hal::SerialPort& port, hal::Clock& clock, std::function<void(uint32_t)> sleepMs);

    bool readConfiguration(Config& config);
    bool writeConfiguration(const Config& config);
    // AOW HAT: one quiet session to change a few settings. The logs are stopped, CONFIG is read
    // and handed to `decide`, which returns the commands to send (none: nothing to change); they
    // are sent, the logs are switched back on, and only then SAVECONFIG, so the saved settings
    // keep their logs. The logs come back on even when a command fails. False if the receiver
    // did not answer or a command failed.
    bool adjustConfiguration(const std::function<std::vector<std::string>(const std::string& configCommands)>& decide);

    static constexpr uint32_t kCommandTimeoutMs = 5000;
    static constexpr uint32_t kSaveConfigTimeoutMs = 10000;

    // Exposed for tests.
    static bool parseConfigResponse(const std::string& response, std::string& out);
    static bool parseModeResponse(const std::string& response, std::string& out);
    static bool parseLogListResponse(const std::string& response, std::string& out);

private:
    uint32_t millis() const { return static_cast<uint32_t>(clock_.nowUs() / 1000); }
    bool sendCommandAndWaitForResponse(const std::string& cmd, std::string& response,
                                       uint32_t timeout = kCommandTimeoutMs);
    bool readLineWithTimeout(std::string& line, uint32_t timeoutMs);
    bool available();
    int readByte();
    void flush();

    hal::SerialPort& port_;
    hal::Clock& clock_;
    std::function<void(uint32_t)> sleep_;
    int pending_ = -1;  // one byte read ahead by available()
};

}  // namespace aow
