// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
// Ported from Firmware_Teensy_AiO-New-Dawn, copyright 2025 by the AOG Group.

#include "aow/indicators.h"

namespace aow::indicators {

namespace {

constexpr uint64_t kBootMs = 5000;
constexpr uint64_t kPulseMs = 50;
constexpr uint64_t kRtcmPulseSpacingMs = 200;

// The firmware's 12-bit colours (PCA9685), R G B.
constexpr uint16_t kColor[5][3] = {{0, 0, 0}, {4095, 0, 0}, {4095, 2048, 0}, {0, 4095, 0}, {0, 0, 4095}};

}  // namespace

void Leds::update(const Inputs& in) {
    const bool booted = in.nowMs - bootMs_ > kBootMs;
    color_[Pwr] = !booted || !in.networkUp ? Color::Red : in.appConnected ? Color::Green : Color::Yellow;
    color_[Gps] = !in.gnssData ? Color::Red : in.fixQuality == 4 ? Color::Green : Color::Yellow;
    color_[Steer] = in.motorActive ? Color::Green : Color::Yellow;
    color_[Ins] = !in.hasIns ? Color::Off : in.insAlignment == 3 ? Color::Green : Color::Yellow;
    for (int i = 0; i < 4; ++i)
        if (pulsing_[i] && in.nowMs - pulseAt_[i] >= kPulseMs) pulsing_[i] = false;
}

void Leds::pulseRtcm(uint64_t nowMs) {
    if (pulsing_[Gps] || nowMs - lastRtcmPulse_ < kRtcmPulseSpacingMs) return;
    lastRtcmPulse_ = nowMs;
    pulsing_[Gps] = true;
    pulseAt_[Gps] = nowMs;
}

void Leds::pulseButton(uint64_t nowMs) {
    pulsing_[Steer] = true;
    pulseAt_[Steer] = nowMs;
}

Rgb Leds::rgb(Led id, uint8_t brightness, uint64_t nowMs) const {
    const bool pulse = pulsing_[id] && nowMs - pulseAt_[id] < kPulseMs;
    const Color c = pulse ? Color::Blue : color_[id];
    uint32_t r = kColor[static_cast<int>(c)][0] * brightness / 100u;
    uint32_t g = kColor[static_cast<int>(c)][1] * brightness / 100u;
    const uint32_t b = kColor[static_cast<int>(c)][2] * brightness / 100u;
    if (c == Color::Red) {
        r = r * 80 / 100;
    } else if (c == Color::Yellow) {
        r = r * 60 / 100;
        g = g * 60 / 100;
    }
    // 12-bit to the SK6812's 8 bits.
    return {static_cast<uint8_t>(r * 255 / 4095), static_cast<uint8_t>(g * 255 / 4095),
            static_cast<uint8_t>(b * 255 / 4095)};
}

std::vector<Tone> bootChime(uint8_t volume) {
    if (volume == 2) return {};
    if (volume == 0) return {{4000, 5, 0}};
    return {{1000, 200, 50}, {1500, 200, 50}, {2000, 300, 50}, {1500, 200, 50}, {1000, 300, 50}};
}

}  // namespace aow::indicators
