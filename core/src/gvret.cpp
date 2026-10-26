// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
// Ported from Firmware_Teensy_AiO-New-Dawn, copyright 2025 by the AOG Group.

#include "aow/gvret.h"

namespace aow::gvret {

namespace {

constexpr uint8_t kBuildCanFrame = 0x00, kTimeSync = 0x01, kGetDigInputs = 0x02, kGetAnalogInputs = 0x03,
                  kSetDigOutputs = 0x04, kGetCanbusParams = 0x06, kGetDevInfo = 0x07, kSetSingleWire = 0x08,
                  kKeepalive = 0x09, kSetSysType = 0x0A, kEchoCanFrame = 0x0B, kGetNumBuses = 0x0C,
                  kGetExtBuses = 0x0D, kSetExtBuses = 0x0E;
constexpr uint8_t kMarker = 0xF1, kBinaryMode = 0xE7;
constexpr uint16_t kBuildNumber = 6020;
constexpr uint8_t kDeviceType = 7;

void le32(std::string& o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o += static_cast<char>((v >> (8 * i)) & 0xFF);
}

}  // namespace

void encodeFrame(std::string& out, uint8_t bus, uint32_t id, bool extended, const uint8_t* data, uint8_t len,
                 uint32_t nowMicros) {
    if (len > 8) len = 8;
    out += static_cast<char>(kMarker);
    out += static_cast<char>(kBuildCanFrame);
    le32(out, nowMicros);
    le32(out, extended ? (id | 0x80000000u) : id);
    out += static_cast<char>((len & 0x0F) | ((bus & 0x0F) << 4));
    out.append(reinterpret_cast<const char*>(data), len);
    out += '\0';
}

std::string Protocol::input(const uint8_t* data, size_t len, uint32_t now) {
    std::string out;
    for (size_t i = 0; i < len; ++i) {
        const uint8_t b = data[i];
        switch (state_) {
        case Idle:
            if (b == kBinaryMode) binary_ = true;
            else if (b == kMarker) state_ = GetCommand;
            break;
        case GetCommand:
            state_ = Idle;
            switch (b) {
            case kTimeSync:
                out += static_cast<char>(kMarker);
                out += static_cast<char>(kTimeSync);
                le32(out, now);
                break;
            case kGetDigInputs: out += std::string("\xF1\x02\x00\x00", 4); break;
            case kGetAnalogInputs: out += std::string("\xF1\x03", 2) + std::string(8, '\0'); break;
            case kSetDigOutputs: break;
            case kGetCanbusParams:  // three buses, all reported enabled at 500k, as the firmware
                out += static_cast<char>(kMarker);
                out += static_cast<char>(kGetCanbusParams);
                for (int bus = 0; bus < 3; ++bus) {
                    out += '\x01';
                    le32(out, 500000);
                }
                break;
            case kGetDevInfo:
                out += static_cast<char>(kMarker);
                out += static_cast<char>(kGetDevInfo);
                out += static_cast<char>(kBuildNumber & 0xFF);
                out += static_cast<char>(kBuildNumber >> 8);
                out += '\x20';
                out += '\0';
                out += static_cast<char>(kDeviceType);
                break;
            case kSetSingleWire: state_ = SetSingleWire; break;
            case kKeepalive: out += std::string("\xF1\x09\xDE\xAD", 4); break;
            case kSetSysType:
            case kEchoCanFrame: break;
            case kGetNumBuses: out += std::string("\xF1\x0C\x03", 3); break;
            case kGetExtBuses: out += std::string("\xF1\x0D\x00\x00", 4); break;
            case kSetExtBuses: step_ = 0; state_ = SetupExtBuses; break;
            case kBuildCanFrame: step_ = 0; state_ = BuildCanFrame; break;
            default: break;
            }
            break;
        case BuildCanFrame:  // id(4) len|bus(1) data checksum: parsed and dropped
            cmd_[step_++] = b;
            if ((step_ >= 5 && step_ >= 5 + (cmd_[4] & 0x0F) + 1) || step_ >= sizeof cmd_) state_ = Idle;
            break;
        case SetSingleWire: state_ = Idle; break;
        case SetupExtBuses:
            cmd_[step_++] = b;
            if (step_ >= 2) state_ = Idle;
            break;
        }
    }
    return out;
}

}  // namespace aow::gvret
