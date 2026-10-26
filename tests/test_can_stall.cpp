// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors

#include "aow/can_stall.h"

#include "check.h"

using namespace aow;

static CanSample sample(uint64_t tx, bool queued, bool active, uint16_t txErr) {
    CanSample s;
    s.txPackets = tx;
    s.queued = queued;
    s.errorActive = active;
    s.txErrors = txErr;
    return s;
}

int main() {
    // The stuck channel: frames queued, none sent, error-active with a clean error counter.
    {
        CanStallDetector d;
        CHECK(!d.update(sample(100, true, true, 0)));  // the first sample has nothing to compare with
        CHECK(!d.update(sample(100, true, true, 0)));
        CHECK(!d.update(sample(100, true, true, 0)));
        CHECK(d.update(sample(100, true, true, 0)));   // three in a row
        // Left alone for ten samples, then it may act again.
        for (int i = 0; i < CanStallDetector::kHoldOffSamples; ++i) CHECK(!d.update(sample(100, true, true, 0)));
        CHECK(!d.update(sample(100, true, true, 0)));
        CHECK(!d.update(sample(100, true, true, 0)));
        CHECK(d.update(sample(100, true, true, 0)));
    }
    // Alone on its bus: queued and nothing sent, but error-passive at 128. Not a stall.
    {
        CanStallDetector d;
        for (int i = 0; i < 20; ++i) CHECK(!d.update(sample(5, true, false, 128)));
    }
    // Error-active but with transmit errors counting up (a bus fault in progress): not yet.
    {
        CanStallDetector d;
        for (int i = 0; i < 20; ++i) CHECK(!d.update(sample(5, true, true, 40)));
    }
    // Idle: nothing queued.
    {
        CanStallDetector d;
        for (int i = 0; i < 20; ++i) CHECK(!d.update(sample(5, false, true, 0)));
    }
    // Busy and healthy: frames queued at each look, but the count moves.
    {
        CanStallDetector d;
        for (uint64_t i = 0; i < 20; ++i) CHECK(!d.update(sample(100 + i, true, true, 0)));
    }
    // A stall that clears by itself before the third sample starts the count again.
    {
        CanStallDetector d;
        d.update(sample(100, true, true, 0));
        CHECK(!d.update(sample(100, true, true, 0)));
        CHECK(!d.update(sample(100, true, true, 0)));
        CHECK(!d.update(sample(101, true, true, 0)));
        CHECK(!d.update(sample(101, true, true, 0)));
        CHECK(!d.update(sample(101, true, true, 0)));
        CHECK(d.update(sample(101, true, true, 0)));
    }
    return finish("can_stall");
}
