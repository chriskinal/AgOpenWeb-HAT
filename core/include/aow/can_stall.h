// Spots a CAN channel that has stopped sending for no reason the bus explains.
//
// Seen on the HAT (docs/PLAN.md section 8, real CAN record): after a burst of bus-off restarts
// an MCP251863 channel sat error-active with frames queued and sent none of them, until the
// interface was taken down and up. A healthy channel with nobody else on its bus also has
// frames queued and none sent, but its transmit error counter is at 128 and its state is
// error-passive: that one is left alone, because cycling it changes nothing.
#pragma once

#include <cstdint>

namespace aow {

struct CanSample {
    uint64_t txPackets = 0;     // frames sent and acknowledged, ever
    bool queued = false;        // frames are waiting to go out
    bool errorActive = false;   // the controller's state is error-active
    uint16_t txErrors = 0;      // its transmit error counter
};

class CanStallDetector {
public:
    static constexpr int kStalledSamples = 3;   // this many in a row, then act
    static constexpr int kHoldOffSamples = 10;  // and leave it alone this long afterwards

    // Feed one sample a second. True when the interface should be taken down and up.
    bool update(const CanSample& s) {
        const bool stuck = have_ && s.queued && s.txPackets == last_ && s.errorActive && s.txErrors == 0;
        last_ = s.txPackets;
        have_ = true;
        if (holdOff_ > 0) {
            --holdOff_;
            stalled_ = 0;
            return false;
        }
        stalled_ = stuck ? stalled_ + 1 : 0;
        if (stalled_ < kStalledSamples) return false;
        stalled_ = 0;
        holdOff_ = kHoldOffSamples;
        return true;
    }

private:
    uint64_t last_ = 0;
    bool have_ = false;
    int stalled_ = 0;
    int holdOff_ = 0;
};

}  // namespace aow
