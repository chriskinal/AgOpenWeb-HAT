#include "aow/loop_timing.h"

#include <algorithm>

namespace aow {

namespace {
uint32_t clampUs(uint64_t us) { return us > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(us); }
}  // namespace

LoopTiming::LoopTiming(uint32_t periodUs, uint32_t windowTicks) : windowTicks_(windowTicks) {
    stats_.periodUs = periodUs;
    stats_.windowS = static_cast<uint32_t>(static_cast<uint64_t>(periodUs) * windowTicks / 1'000'000u);
}

void LoopTiming::start(uint64_t startUs) {
    startUs_ = startUs;
    lastTicks_ = 0;
}

void LoopTiming::record(uint64_t ticks, uint64_t beginUs, uint64_t inputsBeginUs, uint64_t inputsEndUs,
                        uint64_t endUs) {
    const uint64_t dueUs = startUs_ + ticks * stats_.periodUs;
    const uint32_t late = clampUs(beginUs > dueUs ? beginUs - dueUs : 0);
    const uint32_t work = clampUs(endUs - beginUs);
    const uint32_t inputs = clampUs(inputsEndUs - inputsBeginUs);

    if (ticks > lastTicks_ + 1) stats_.missed += static_cast<uint32_t>(ticks - lastTicks_ - 1);
    lastTicks_ = ticks;
    stats_.ticks = ticks;
    if (late > 1000) ++stats_.lateOver1ms;

    lateMax_ = std::max(lateMax_, late);
    workMax_ = std::max(workMax_, work);
    inputsMax_ = std::max(inputsMax_, inputs);
    stats_.lateMaxEverUs = std::max(stats_.lateMaxEverUs, late);
    stats_.workMaxEverUs = std::max(stats_.workMaxEverUs, work);
    stats_.inputsMaxEverUs = std::max(stats_.inputsMaxEverUs, inputs);

    if (++inWindow_ >= windowTicks_) {
        stats_.lateMaxUs = lateMax_;
        stats_.workMaxUs = workMax_;
        stats_.inputsMaxUs = inputsMax_;
        lateMax_ = workMax_ = inputsMax_ = 0;
        inWindow_ = 0;
    }
}

}  // namespace aow
