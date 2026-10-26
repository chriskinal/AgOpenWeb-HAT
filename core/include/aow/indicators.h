// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
// Ported from Firmware_Teensy_AiO-New-Dawn, copyright 2025 by the AOG Group.
//
// Status LEDs and the buzzer, ported from the firmware's LEDManagerFSM and
// HardwareManager::performBuzzerTest. Pure logic: the board draws the colours and plays the tones.
//
//   LED 0 PWR    red booting (first 5 s) or no network, amber network up, green app talking
//   LED 1 GPS    red no GNSS data for 5 s, amber any fix below RTK fixed, green RTK fixed;
//                50 ms blue pulse per RTCM packet (at most every 200 ms)
//   LED 2 STEER  amber motor off, green motor running; 50 ms blue pulse per steer button press
//   LED 3 INS    off without INS, amber aligning, green aligned (UM981 INS)
#pragma once

#include <cstdint>
#include <vector>

namespace aow::indicators {

enum class Color : uint8_t { Off, Red, Yellow, Green, Blue };
enum Led : uint8_t { Pwr = 0, Gps = 1, Steer = 2, Ins = 3 };

struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
    bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; }
};

struct Inputs {
    uint64_t nowMs = 0;
    bool networkUp = false;     // a non-loopback interface is up
    bool appConnected = false;  // a PGN from the app in the last 5 s
    bool gnssData = false;      // a position sentence in the last 5 s
    uint8_t fixQuality = 0;
    bool hasIns = false;
    uint8_t insAlignment = 0;   // 3 = solution good
    bool motorActive = false;
};

class Leds {
public:
    explicit Leds(uint64_t bootMs) : bootMs_(bootMs) {}

    void update(const Inputs& in);
    void pulseRtcm(uint64_t nowMs);
    void pulseButton(uint64_t nowMs);

    Color color(Led id) const { return color_[id]; }
    // What to draw: brightness 5..100 %, the firmware's red and amber dimming, 8-bit channels.
    Rgb rgb(Led id, uint8_t brightnessPercent, uint64_t nowMs) const;

private:
    uint64_t bootMs_;
    Color color_[4] = {Color::Red, Color::Red, Color::Yellow, Color::Off};
    uint64_t pulseAt_[4] = {};
    bool pulsing_[4] = {};
    uint64_t lastRtcmPulse_ = 0;
};

struct Tone {
    uint16_t hz;
    uint16_t ms;     // sounding
    uint16_t gapMs;  // then silence before the next
};

// The boot sound for the Device page's buzzer volume: 0 quiet (a click), 1 loud (the five-tone
// chime), 2 off (nothing).
std::vector<Tone> bootChime(uint8_t volume);

}  // namespace aow::indicators
