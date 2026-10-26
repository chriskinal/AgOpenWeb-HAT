// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
// Ported from Firmware_Teensy_AiO-New-Dawn, copyright 2025 by the AOG Group.
//
// An external IMU on a serial port (on the HAT, an RS-232 port): a BNO085 in RVC mode or a SYD
// TM171, told apart by their packets as the firmware's IMUProcessor does. Ported from the
// firmware's BNOAiOParser, TM171AiOParser and IMUProcessor, with these changes (AOW HAT):
//   - The yaw rate is a real rate, deg/s, measured over 100 ms with the 360 wrap handled. The
//     firmware sums yaw steps over 20 packets and resets with prevYaw = 0, so its first step after
//     each reset is the whole yaw angle; for the TM171 it is always 0.
//   - Roll and pitch are kept in degrees; the x10 for PANDA is applied once, in nav.cpp.
//   - Heading is kept in 0..360 for both IMUs (the TM171's yaw is passed on as sent in the firmware).
//   - The TM171's roll is passed on as sent. The firmware negates it ("inverted roll axis"), which
//     gives right side down negative; the app wants it positive (bench, 2026-10-15, arrow forward).
//   - Detection does not block: bytes go to both parsers until one has three valid packets, at
//     any time after start, not only in the first 600 ms. Three, because the BNO's 8-bit checksum
//     would otherwise pass on one stray "AA AA" in a TM171 stream in 256.
#pragma once

#include <cstddef>
#include <cstdint>

namespace aow::imu {

enum class Type : uint8_t { None, Bno085, Tm171 };
const char* typeName(Type t);

// BNO085 RVC: 0xAA 0xAA, index, yaw, pitch, roll (int16, 0.01 deg), accelerations, 3 reserved,
// checksum (sum of the 16 bytes after the header). 19 bytes at 100 Hz, 115200 baud.
class BnoRvcParser {
public:
    // True when this byte completes a packet with a good checksum.
    bool feed(uint8_t b);
    float yaw() const { return yaw_; }      // deg, -180..180 as sent
    float pitch() const { return pitch_; }  // deg
    float roll() const { return roll_; }    // deg

private:
    uint8_t buf_[19] = {};
    uint8_t len_ = 0;
    float yaw_ = 0, pitch_ = 0, roll_ = 0;
};

// TM171: 0xAA 0x55, size, payload (4 bytes of payload info, the first's low 7 bits the object
// id), CRC-16/Modbus over size and payload, little-endian. Only the roll/pitch/yaw object (0x23,
// 20 bytes: info, timestamp, roll, pitch, yaw as floats in deg) is used; others are skipped.
class Tm171Parser {
public:
    // True when this byte completes a roll/pitch/yaw packet with a good CRC.
    bool feed(uint8_t b);
    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }
    float roll() const { return roll_; }    // as sent: right side down is positive
    uint32_t crcErrors() const { return crcErrors_; }

private:
    static constexpr uint8_t kMax = 128;
    uint8_t buf_[kMax] = {};
    uint8_t len_ = 0;
    float yaw_ = 0, pitch_ = 0, roll_ = 0;
    uint32_t crcErrors_ = 0;
};

struct Data {
    float heading = 0;   // deg, 0..360
    float roll = 0;      // deg
    float pitch = 0;     // deg
    float yawRate = 0;   // deg/s, positive with heading increasing
};

class Imu {
public:
    // Bytes from the serial port, and when they were read.
    void feed(const uint8_t* buf, size_t n, uint64_t nowUs);

    // PGN 251's "use Y axis" bit: the IMU is mounted turned 90 degrees, so roll and pitch swap.
    void setSwapXY(bool swap) { swapXY_ = swap; }

    Type type() const { return type_; }
    // A packet in the last 100 ms, as the firmware.
    bool valid(uint64_t nowUs) const { return packets_ && nowUs - lastUs_ < 100'000; }
    const Data& data() const { return data_; }
    uint32_t packets() const { return packets_; }

private:
    void sample(float yaw, float pitch, float roll, uint64_t nowUs);

    // Unwrapped yaw history for the rate: about 640 ms at 100 Hz.
    static constexpr int kHistory = 64;
    static constexpr uint64_t kRateWindowUs = 100'000;
    struct Point { uint64_t us; double yaw; };
    Point hist_[kHistory] = {};
    int histCount_ = 0, histHead_ = 0;
    double unwrapped_ = 0;
    float lastYaw_ = 0;

    BnoRvcParser bno_;
    Tm171Parser tm_;
    static constexpr uint8_t kDetectPackets = 3;
    uint8_t bnoSeen_ = 0, tmSeen_ = 0;
    Type type_ = Type::None;
    bool swapXY_ = false;
    Data data_;
    uint32_t packets_ = 0;
    uint64_t lastUs_ = 0;
};

}  // namespace aow::imu
