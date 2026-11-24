// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors

#include "aow/cpu_health.h"

#include <initializer_list>

#include "check.h"

using namespace aow;

int main() {
    CpuMonitor m;
    CpuHealth h = m.health(0);
    CHECK(!h.haveTemp && !h.haveThrottled && h.underVoltageAgoS == -1 && h.throttleAgoS == -1);

    // Temperature: now and the highest.
    m.sample(1, true, 61.5f, true, 0);
    m.sample(2, true, 64.0f, true, 0);
    m.sample(3, true, 60.0f, true, 0);
    h = m.health(3);
    CHECK(h.haveTemp && h.tempC == 60.0f && h.tempMaxC == 64.0f);
    CHECK(h.haveThrottled && !h.underVoltage && !h.throttling && h.underVoltageCount == 0);
    m.sample(4, false, 0, true, 0);  // a failed read keeps the last
    CHECK(m.health(4).tempC == 60.0f);

    // Under-voltage now: an episode starts once.
    CpuMonitor::Change c = m.sample(10, true, 60, true, throttle::kUnderVoltage);
    CHECK(c.underVoltageStarted && !c.throttleStarted);
    c = m.sample(11, true, 60, true, throttle::kUnderVoltage);
    CHECK(!c.underVoltageStarted);
    h = m.health(11);
    CHECK(h.underVoltage && h.underVoltageCount == 1 && h.underVoltageAgoS == 0);
    // The "has occurred" bit coming on (the dip ended between samples): still seen.
    c = m.sample(12, true, 60, true, throttle::kUnderVoltage << throttle::kOccurred);
    CHECK(!c.underVoltageStarted && m.health(12).underVoltageAgoS == 0 && m.health(12).underVoltageOccurred);
    // Gone: held for kHoldS, then the episode ends.
    c = m.sample(12 + CpuMonitor::kHoldS - 1, true, 60, true, 0);
    CHECK(!c.underVoltageEnded && m.health(12 + CpuMonitor::kHoldS - 1).underVoltage);
    c = m.sample(12 + CpuMonitor::kHoldS, true, 60, true, 0);
    CHECK(c.underVoltageEnded);
    h = m.health(30);
    CHECK(!h.underVoltage && h.underVoltageCount == 1 && h.underVoltageAgoS == 18);
    // A second dip is a second episode.
    c = m.sample(40, true, 60, true, throttle::kUnderVoltage);
    CHECK(c.underVoltageStarted && m.health(40).underVoltageCount == 2);

    // Throttling: any of capped, throttled, soft limit; now, or occurred since the last sample.
    for (uint32_t bit : {throttle::kFreqCapped, throttle::kThrottled, throttle::kSoftTempLimit}) {
        CpuMonitor t;
        CHECK(t.sample(1, true, 81, true, bit).throttleStarted);
        CpuMonitor o;
        CHECK(!o.sample(1, true, 70, true, 0).throttleStarted);
        CHECK(o.sample(2, true, 81, true, bit << throttle::kOccurred).throttleStarted);
        CHECK(!o.health(2).underVoltage && o.health(2).throttling && o.health(2).throttleOccurred);
    }

    // The occurred bits stay set after it is over (seen on a Pi 4B: 0x80008, then 0x80000 once
    // cool): the episode still ends, and the firmware's record stays.
    CpuMonitor k;
    k.sample(1, true, 70, true, 0);
    CHECK(k.sample(2, true, 82, true, 0x80008).throttleStarted);
    for (uint64_t t = 3; t < 3 + CpuMonitor::kHoldS; ++t) k.sample(t, true, 75, true, 0x80000);
    h = k.health(3 + CpuMonitor::kHoldS);
    CHECK(!h.throttling && h.throttleCount == 1 && h.throttleOccurred && h.throttleAgoS == 1 + static_cast<int64_t>(CpuMonitor::kHoldS));

    // Already set when the daemon starts: the firmware's record, not an episode of ours.
    CpuMonitor late;
    CHECK(!late.sample(1, true, 60, true, 0x80000).throttleStarted);
    h = late.health(1);
    CHECK(!h.throttling && h.throttleCount == 0 && h.throttleAgoS == -1 && h.throttleOccurred);

    // Not a Pi: no firmware answer, nothing about voltage.
    CpuMonitor x;
    x.sample(1, true, 45, false, 0xFFFFFFFF);
    h = x.health(1);
    CHECK(!h.haveThrottled && !h.underVoltage && !h.throttling && h.underVoltageAgoS == -1);

    return finish("cpu_health");
}
