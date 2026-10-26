// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
// Ported from Firmware_Teensy_AiO-New-Dawn, copyright 2025 by the AOG Group.

#include "aow/um98x.h"

#include <cstdlib>
#include <vector>

#include "aow/log.h"

namespace aow {

namespace {

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    const size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        const std::string line = trim(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (!line.empty()) out.push_back(line);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

// "<   3" -> 3; anything else (a log line) -> -1. The firmware's "length <= 3 and numeric".
int logCount(const std::string& afterAngle) {
    const std::string c = trim(afterAngle);
    if (c.empty() || c.size() > 3) return -1;
    for (char ch : c)
        if (ch < '0' || ch > '9') return -1;
    return std::atoi(c.c_str());
}

}  // namespace

Um98x::Um98x(hal::SerialPort& port, hal::Clock& clock, std::function<void(uint32_t)> sleepMs)
    : port_(port), clock_(clock), sleep_(std::move(sleepMs)) {}

bool Um98x::available() {
    if (pending_ >= 0) return true;
    uint8_t b;
    if (port_.read(&b, 1) == 1) pending_ = b;
    return pending_ >= 0;
}

int Um98x::readByte() {
    if (!available()) return -1;
    const int b = pending_;
    pending_ = -1;
    return b;
}

void Um98x::flush() {
    pending_ = -1;
    uint8_t buf[256];
    while (port_.read(buf, sizeof buf) > 0) {}
}

bool Um98x::readLineWithTimeout(std::string& line, uint32_t timeoutMs) {
    line.clear();
    const uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        const int c = readByte();
        if (c < 0) {
            sleep_(1);
            continue;
        }
        if (c == '\r') continue;
        if (c == '\n') {
            if (!line.empty()) return true;
        } else {
            line += static_cast<char>(c);
            if (line.size() > 512) return true;
        }
    }
    return false;
}

bool Um98x::sendCommandAndWaitForResponse(const std::string& cmd, std::string& response, uint32_t timeout) {
    flush();
    const std::string out = cmd + "\r\n";
    port_.write(reinterpret_cast<const uint8_t*>(out.data()), out.size());
    response.clear();
    const bool query = cmd == "CONFIG" || cmd == "MODE" || cmd == "UNILOGLIST";
    const uint32_t start = millis();
    bool gotAck = false;
    uint32_t lastConfigTime = 0;
    std::string line;
    while (millis() - start < timeout) {
        if (!readLineWithTimeout(line, 100)) continue;
        if (startsWith(line, "$command,") && line.find("response: OK") != std::string::npos) {
            gotAck = true;
            response += line + "\n";
            if (!query) return true;
            lastConfigTime = millis();
        } else if (cmd == "CONFIG" && startsWith(line, "$CONFIG,")) {
            response += line + "\n";
            lastConfigTime = millis();
            if (startsWith(line, "$CONFIG,COM3,")) {  // usually the last
                sleep_(100);
                if (!available()) return true;
            }
        } else if (cmd == "MODE" && startsWith(line, "#MODE,")) {
            response += line + "\n";
            return true;
        } else if (cmd == "UNILOGLIST" && startsWith(line, "<")) {
            response += line + "\n";
            const int count = logCount(line.substr(1));
            if (count >= 0) {
                if (count == 0) return true;
                int read = 0;
                while (read < count && millis() - start < timeout)
                    if (readLineWithTimeout(line, 100)) {
                        response += line + "\n";
                        ++read;
                    }
                if (read == count) return true;
            } else {
                lastConfigTime = millis();
            }
        } else if (gotAck && (cmd == "CONFIG" || cmd == "UNILOGLIST")) {
            if (lastConfigTime > 0 && millis() - lastConfigTime > 300) return true;
        }
    }
    return query && gotAck;
}

bool Um98x::parseConfigResponse(const std::string& response, std::string& out) {
    out.clear();
    for (const std::string& line : lines(response)) {
        if (!startsWith(line, "$CONFIG,")) continue;
        const size_t first = line.find(',');
        const size_t second = line.find(',', first + 1);
        if (second == std::string::npos) continue;
        const size_t star = line.find('*');
        const std::string cmd = trim(star != std::string::npos && star > second ? line.substr(second + 1, star - second - 1)
                                                                               : line.substr(second + 1));
        if (cmd.empty()) continue;
        if (!out.empty()) out += "\n";
        out += cmd;
    }
    return !out.empty();
}

bool Um98x::parseModeResponse(const std::string& response, std::string& out) {
    out.clear();
    const size_t start = response.find("#MODE,");
    if (start == std::string::npos) return false;
    const size_t end = response.find('\n', start);
    const std::string line = response.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const size_t semi = line.find(';');
    if (semi == std::string::npos || semi == 0) return false;
    const size_t star = line.find('*', semi);
    if (star != std::string::npos) {
        out = line.substr(semi + 1, star - semi - 1);
        if (!out.empty() && out.back() == ',') out.pop_back();
    } else {
        out = line.substr(semi + 1);
    }
    out = trim(out);
    return true;
}

bool Um98x::parseLogListResponse(const std::string& response, std::string& out) {
    out.clear();
    for (const std::string& line : lines(response)) {
        if (!startsWith(line, "<")) continue;
        const std::string content = trim(line.substr(1));
        if (logCount(content) >= 0) continue;  // the count line
        if (content.empty()) continue;
        if (!out.empty()) out += "\n";
        out += content;
    }
    return true;
}

bool Um98x::readConfiguration(Config& config) {
    config = Config{};
    std::string response, saved;
    // Remember the current logs so they can be restored after the quiet read.
    if (sendCommandAndWaitForResponse("UNILOGLIST", response)) parseLogListResponse(response, saved);
    for (const char* c : {"UNLOGALL COM1", "UNLOGALL COM2", "UNLOGALL COM3"}) sendCommandAndWaitForResponse(c, response);
    sleep_(100);

    bool ok = sendCommandAndWaitForResponse("CONFIG", response) && parseConfigResponse(response, config.configCommands);
    if (!ok) LOG_ERROR(Gnss, "UM98x: CONFIG read failed");
    if (ok) {
        bool mode = false;
        for (int attempt = 0; attempt < 3 && !mode; ++attempt) {
            if (attempt) sleep_(100);
            mode = sendCommandAndWaitForResponse("MODE", response) && parseModeResponse(response, config.modeSettings);
        }
        ok = mode;
        if (!ok) LOG_ERROR(Gnss, "UM98x: MODE read failed");
    }
    if (ok) {
        ok = sendCommandAndWaitForResponse("UNILOGLIST", response) &&
             parseLogListResponse(response, config.messageSettings);
        if (ok && config.messageSettings.empty() && !saved.empty()) config.messageSettings = saved;
    }
    if (ok)
        for (const std::string& cmd : lines(saved)) sendCommandAndWaitForResponse(cmd, response);  // logs back on
    return ok;
}

bool Um98x::writeConfiguration(const Config& config) {
    std::string response;
    for (const char* c : {"UNLOGALL COM1", "UNLOGALL COM2", "UNLOGALL COM3"}) sendCommandAndWaitForResponse(c, response);
    sleep_(100);
    bool ok = true;
    for (const std::string& cmd : lines(config.configCommands))
        if (!(ok = sendCommandAndWaitForResponse(cmd, response))) {
            LOG_ERROR(Gnss, "UM98x: '%s' failed", cmd.c_str());
            break;
        }
    const std::string mode = trim(config.modeSettings);
    if (ok && !mode.empty() && !(ok = sendCommandAndWaitForResponse(mode, response)))
        LOG_ERROR(Gnss, "UM98x: '%s' failed", mode.c_str());
    if (ok) {
        for (const char* c : {"UNLOGALL COM1", "UNLOGALL COM2", "UNLOGALL COM3"}) sendCommandAndWaitForResponse(c, response);
        for (const std::string& cmd : lines(config.messageSettings))
            if (!(ok = sendCommandAndWaitForResponse(cmd, response))) {
                LOG_ERROR(Gnss, "UM98x: '%s' failed", cmd.c_str());
                break;
            }
    }
    if (ok && !(ok = sendCommandAndWaitForResponse("SAVECONFIG", response, kSaveConfigTimeoutMs)))
        LOG_ERROR(Gnss, "UM98x: SAVECONFIG failed");
    if (ok) LOG_INFO(Gnss, "UM98x configuration written and saved");
    return ok;
}

}  // namespace aow
