// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
// Ported from Firmware_Teensy_AiO-New-Dawn, copyright 2025 by the AOG Group.
//
// GVRET, SavvyCAN's protocol, ported from the firmware's GVRETServer: the device answers
// SavvyCAN's queries and streams mirrored CAN frames in binary mode. As in the firmware, frames
// sent from SavvyCAN (BUILD_CAN_FRAME) are parsed and dropped. Transport-free: bytes in, bytes out.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace aow::gvret {

class Protocol {
public:
    // Bytes from SavvyCAN. Returns what to send back.
    std::string input(const uint8_t* data, size_t len, uint32_t nowMicros);
    // Binary mode (SavvyCAN sends 0xE7 first): only then are frames streamed.
    bool binaryMode() const { return binary_; }
    void reset() { *this = Protocol(); }

private:
    enum State : uint8_t { Idle, GetCommand, BuildCanFrame, SetSingleWire, SetupExtBuses };
    State state_ = Idle;
    bool binary_ = false;
    uint8_t cmd_[20] = {};
    uint8_t step_ = 0;
};

// One frame in GVRET's binary form: F1 00 ts(4) id(4, bit 31 = extended) len|bus<<4 data 00.
void encodeFrame(std::string& out, uint8_t bus, uint32_t id, bool extended, const uint8_t* data, uint8_t len,
                 uint32_t nowMicros);

}  // namespace aow::gvret
