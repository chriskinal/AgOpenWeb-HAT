// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
// Ported from Firmware_Teensy_AiO-New-Dawn, copyright 2025 by the AOG Group.

#include "aow/imu.h"

#include <cmath>
#include <cstring>

namespace aow::imu {

namespace {

int16_t le16(const uint8_t* p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }

float leFloat(const uint8_t* p) {
    float f;
    std::memcpy(&f, p, sizeof f);
    return f;
}

uint16_t crc16Modbus(const uint8_t* d, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (int j = 0; j < 8; ++j) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

// -180..180
float wrap180(float deg) {
    while (deg > 180.0f) deg -= 360.0f;
    while (deg < -180.0f) deg += 360.0f;
    return deg;
}

}  // namespace

const char* typeName(Type t) {
    switch (t) {
    case Type::Bno085: return "BNO085";
    case Type::Tm171: return "TM171";
    case Type::None: break;
    }
    return "None";
}

bool BnoRvcParser::feed(uint8_t b) {
    if (len_ == 0) {
        if (b == 0xAA) buf_[len_++] = b;
        return false;
    }
    if (len_ == 1) {
        if (b == 0xAA) buf_[len_++] = b;
        else len_ = 0;
        return false;
    }
    buf_[len_++] = b;
    if (len_ < sizeof buf_) return false;
    len_ = 0;
    uint8_t sum = 0;
    for (int i = 2; i < 18; ++i) sum = static_cast<uint8_t>(sum + buf_[i]);
    if (sum != buf_[18]) return false;
    yaw_ = le16(&buf_[3]) * 0.01f;
    pitch_ = le16(&buf_[5]) * 0.01f;
    roll_ = le16(&buf_[7]) * 0.01f;
    return true;
}

bool Tm171Parser::feed(uint8_t b) {
    if (len_ == 0) {
        if (b == 0xAA) buf_[len_++] = b;
        return false;
    }
    if (len_ == 1) {
        if (b == 0x55) buf_[len_++] = b;
        else len_ = 0;
        return false;
    }
    buf_[len_++] = b;
    const uint8_t size = buf_[2];
    const size_t total = 3u + size + 2u;  // header, size, payload, CRC
    if (total > kMax) {
        len_ = 0;
        return false;
    }
    if (len_ < total) return false;
    len_ = 0;
    const uint16_t crc = static_cast<uint16_t>(buf_[total - 2] | (buf_[total - 1] << 8));
    if (crc16Modbus(&buf_[2], total - 4) != crc) {
        ++crcErrors_;
        return false;
    }
    constexpr uint8_t kRpyObject = 0x23, kRpySize = 20;
    if ((buf_[3] & 0x7F) != kRpyObject || size != kRpySize) return false;
    // [3..6] payload info, [7..10] timestamp, then roll, pitch, yaw.
    roll_ = leFloat(&buf_[11]);
    pitch_ = leFloat(&buf_[15]);
    yaw_ = leFloat(&buf_[19]);
    return true;
}

void Imu::feed(const uint8_t* buf, size_t n, uint64_t nowUs) {
    for (size_t i = 0; i < n; ++i) {
        const uint8_t b = buf[i];
        if (type_ != Type::Tm171 && bno_.feed(b) && (type_ == Type::Bno085 || ++bnoSeen_ >= kDetectPackets)) {
            type_ = Type::Bno085;
            sample(bno_.yaw(), bno_.pitch(), bno_.roll(), nowUs);
        }
        if (type_ != Type::Bno085 && tm_.feed(b) && (type_ == Type::Tm171 || ++tmSeen_ >= kDetectPackets)) {
            type_ = Type::Tm171;
            sample(tm_.yaw(), tm_.pitch(), tm_.roll(), nowUs);
        }
    }
}

void Imu::sample(float yaw, float pitch, float roll, uint64_t nowUs) {
    // A gap of half a second or more: the old history says nothing about the rate now.
    if (packets_ && nowUs - lastUs_ > 500'000) histCount_ = 0;
    unwrapped_ = histCount_ ? unwrapped_ + wrap180(yaw - lastYaw_) : yaw;
    lastYaw_ = yaw;
    histHead_ = (histHead_ + 1) % kHistory;
    hist_[histHead_] = {nowUs, unwrapped_};
    if (histCount_ < kHistory) ++histCount_;

    // Rate against the newest point at least 100 ms old; 0 until there is one.
    data_.yawRate = 0;
    for (int k = 1; k < histCount_; ++k) {
        const Point& p = hist_[(histHead_ - k + kHistory) % kHistory];
        if (nowUs - p.us >= kRateWindowUs) {
            data_.yawRate = static_cast<float>((unwrapped_ - p.yaw) / ((nowUs - p.us) * 1e-6));
            break;
        }
    }

    float h = std::fmod(yaw, 360.0f);
    if (h < 0) h += 360.0f;
    data_.heading = h;
    data_.roll = swapXY_ ? pitch : roll;
    data_.pitch = swapXY_ ? roll : pitch;
    ++packets_;
    lastUs_ = nowUs;
}

}  // namespace aow::imu
