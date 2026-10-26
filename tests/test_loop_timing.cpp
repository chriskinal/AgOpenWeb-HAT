// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors

#include "aow/loop_timing.h"

#include "check.h"

using aow::LoopTiming;

int main() {
    LoopTiming t(10'000, 3);  // 10 ms ticks, a 3-tick window
    CHECK(t.stats().periodUs == 10'000);
    t.start(1'000'000);

    // Tick 1 due at 1.010 s, starts 200 us late, reads inputs for 50 us, ends 300 us in.
    t.record(1, 1'010'200, 1'010'250, 1'010'300, 1'010'500);
    CHECK(t.stats().ticks == 1);
    CHECK(t.stats().missed == 0);
    CHECK(t.stats().lateMaxEverUs == 200);
    CHECK(t.stats().workMaxEverUs == 300);
    CHECK(t.stats().inputsMaxEverUs == 50);
    CHECK(t.stats().lateMaxUs == 0);  // the window is not full yet

    // Tick 2 starts 1.5 ms late.
    t.record(2, 1'021'500, 1'021'500, 1'021'510, 1'021'600);
    CHECK(t.stats().lateOver1ms == 1);

    // Two ticks lost: the timer read says 5. On time otherwise; the window (3 passes) closes.
    t.record(5, 1'050'000, 1'050'000, 1'050'020, 1'050'100);
    CHECK(t.stats().missed == 2);
    CHECK(t.stats().lateMaxUs == 1500);
    CHECK(t.stats().workMaxUs == 300);
    CHECK(t.stats().inputsMaxUs == 50);

    // The next window starts afresh; the since-start worst cases stay.
    t.record(6, 1'060'010, 1'060'010, 1'060'020, 1'060'030);
    t.record(7, 1'070'010, 1'070'010, 1'070'020, 1'070'030);
    t.record(8, 1'080'010, 1'080'010, 1'080'020, 1'080'030);
    CHECK(t.stats().lateMaxUs == 10);
    CHECK(t.stats().workMaxUs == 20);
    CHECK(t.stats().lateMaxEverUs == 1500);
    CHECK(t.stats().missed == 2);

    // Early (timer and clock a few us apart) counts as on time.
    t.record(9, 1'089'990, 1'089'990, 1'090'000, 1'090'010);
    CHECK(t.stats().lateMaxEverUs == 1500);
    return finish("loop_timing");
}
