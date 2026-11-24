// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
//
// The host computer's health for the home page: the CPU's temperature, and what the Pi's firmware
// reports as under-voltage and throttling (its get_throttled bits). The monitor is sampled every
// second and remembers when it last saw each, as episodes that end after kHoldS seconds without
// one. "Seen" is a "now" bit (0-3), or a "has occurred" bit (16-19) that came on since the last
// sample: a dip shorter than a second. The occurred bits are sticky (on the Pi 4B, Trixie, a soft
// temperature limit stayed in them after the CPU had cooled), and a reader that sends 0xffff
// clears them, so they are used only as edges, and reported as the firmware's own record.
#pragma once

#include <cstdint>

namespace aow {

namespace throttle {
// get_throttled: bits 0-3 now, the same at bit + kOccurred since the last clear.
constexpr uint32_t kUnderVoltage = 1u << 0;  // the 5 V supply below about 4.63 V
constexpr uint32_t kFreqCapped = 1u << 1;
constexpr uint32_t kThrottled = 1u << 2;
constexpr uint32_t kSoftTempLimit = 1u << 3;  // 80 °C on a Pi 4
constexpr int kOccurred = 16;
}  // namespace throttle

constexpr float kCpuWarmC = 75.0f;  // shown in amber: the firmware starts slowing the CPU at 80 °C

struct CpuHealth {
    bool haveTemp = false;      // the thermal zone was read
    float tempC = 0, tempMaxC = 0;  // now, and the highest since start
    bool haveThrottled = false;  // the firmware answered (a Raspberry Pi)
    // Each: there now (within kHoldS), how many episodes since start, seconds since the last
    // one was seen (-1: never).
    bool underVoltage = false, throttling = false;  // throttling: capped, throttled or soft limit
    // The firmware's "has occurred" bits as last read: since boot, unless a reader cleared them.
    bool underVoltageOccurred = false, throttleOccurred = false;
    uint32_t underVoltageCount = 0, throttleCount = 0;
    int64_t underVoltageAgoS = -1, throttleAgoS = -1;
};

class CpuMonitor {
public:
    static constexpr uint64_t kHoldS = 10;

    // What a sample started or ended, for the log.
    struct Change {
        bool underVoltageStarted = false, underVoltageEnded = false;
        bool throttleStarted = false, throttleEnded = false;
    };

    // One sample, every second; nowS is monotonic.
    Change sample(uint64_t nowS, bool haveTemp, float tempC, bool haveThrottled, uint32_t bits);
    CpuHealth health(uint64_t nowS) const;

private:
    struct Episode {
        bool active = false;
        bool seen = false;  // ever
        uint32_t count = 0;
        uint64_t lastS = 0;
        // Started (true) or ended (false) by this sample; neither: unchanged.
        void update(uint64_t nowS, bool now, bool& started, bool& ended);
    };
    CpuHealth h_;
    Episode underVoltage_, throttle_;
    bool haveOccurred_ = false;
    uint32_t occurred_ = 0;  // the last sample's bits 16-19, shifted down
};

}  // namespace aow
