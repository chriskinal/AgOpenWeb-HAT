// How well the control loop keeps its 100 Hz tick: how late each tick starts against its
// schedule, how long its work takes, how long reading the board's inputs takes (the ADC on
// SPI0, which the CAN controllers share), and ticks missed outright. Plain numbers for
// /api/status: worst cases over the last full window and since start.
#pragma once

#include <cstdint>

namespace aow {

struct LoopStats {
    uint32_t periodUs = 0;
    uint64_t ticks = 0;
    uint32_t missed = 0;      // ticks skipped: the loop was a whole period or more behind
    uint32_t lateOver1ms = 0; // ticks that started more than 1 ms late
    // Worst cases in the last full window (windowS seconds) and since start, in us.
    uint32_t windowS = 0;
    uint32_t lateMaxUs = 0, workMaxUs = 0, inputsMaxUs = 0;
    uint32_t lateMaxEverUs = 0, workMaxEverUs = 0, inputsMaxEverUs = 0;
};

class LoopTiming {
public:
    LoopTiming(uint32_t periodUs, uint32_t windowTicks);

    // The tick timer was armed at startUs: tick n is due at startUs + n * period.
    void start(uint64_t startUs);
    // One pass of the tick's work. ticks: the count after this pass's timer read (it grows by
    // more than one when ticks were missed). beginUs: when the pass began; inputsUs and
    // endUs: when the input read finished, and when the pass ended.
    void record(uint64_t ticks, uint64_t beginUs, uint64_t inputsBeginUs, uint64_t inputsEndUs, uint64_t endUs);

    const LoopStats& stats() const { return stats_; }

private:
    uint64_t startUs_ = 0;
    uint64_t lastTicks_ = 0;
    uint32_t windowTicks_;
    uint32_t inWindow_ = 0;
    uint32_t lateMax_ = 0, workMax_ = 0, inputsMax_ = 0;  // the window being filled
    LoopStats stats_;
};

}  // namespace aow
