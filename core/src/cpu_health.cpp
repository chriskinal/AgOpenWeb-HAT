// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors

#include "aow/cpu_health.h"

namespace aow {

void CpuMonitor::Episode::update(uint64_t nowS, bool now, bool& started, bool& ended) {
    if (now) {
        if (!active) {
            active = true;
            ++count;
            started = true;
        }
        seen = true;
        lastS = nowS;
    } else if (active && nowS - lastS >= kHoldS) {
        active = false;
        ended = true;
    }
}

CpuMonitor::Change CpuMonitor::sample(uint64_t nowS, bool haveTemp, float tempC, bool haveThrottled, uint32_t bits) {
    Change c;
    if (haveTemp) {
        if (!h_.haveTemp || tempC > h_.tempMaxC) h_.tempMaxC = tempC;
        h_.haveTemp = true;
        h_.tempC = tempC;
    }
    if (haveThrottled) {
        constexpr uint32_t kSlow = throttle::kFreqCapped | throttle::kThrottled | throttle::kSoftTempLimit;
        h_.haveThrottled = true;
        const uint32_t occurred = (bits >> throttle::kOccurred) & 0xF;
        // An occurred bit already on at the first sample happened before this daemon started.
        const uint32_t fresh = haveOccurred_ ? occurred & ~occurred_ : 0;
        haveOccurred_ = true;
        occurred_ = occurred;
        h_.underVoltageOccurred = occurred & throttle::kUnderVoltage;
        h_.throttleOccurred = occurred & kSlow;
        const uint32_t seen = (bits & 0xF) | fresh;
        underVoltage_.update(nowS, seen & throttle::kUnderVoltage, c.underVoltageStarted, c.underVoltageEnded);
        throttle_.update(nowS, seen & kSlow, c.throttleStarted, c.throttleEnded);
    }
    return c;
}

CpuHealth CpuMonitor::health(uint64_t nowS) const {
    CpuHealth h = h_;
    h.underVoltage = underVoltage_.active;
    h.throttling = throttle_.active;
    h.underVoltageCount = underVoltage_.count;
    h.throttleCount = throttle_.count;
    h.underVoltageAgoS = underVoltage_.seen ? static_cast<int64_t>(nowS - underVoltage_.lastS) : -1;
    h.throttleAgoS = throttle_.seen ? static_cast<int64_t>(nowS - throttle_.lastS) : -1;
    return h;
}

}  // namespace aow
