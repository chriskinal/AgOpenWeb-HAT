// The HAT's four SK6812 status LEDs (D19-D22) on GPIO21, clocked out by the PCM block.
//
// The PCM bit clock runs at 3 MHz and each LED bit is four PCM bits: 1000 for a 0 (0.33 us
// high, 1.0 us low) and 1100 for a 1 (0.67 us high, 0.67 us low), inside the SK6812's limits.
// Four LEDs are 12 words, which fit in the PCM's 64-word FIFO, so a frame is loaded whole and
// then shifted out by the hardware: no DMA, and the timing does not depend on the scheduler.
// Needs /dev/mem (root). The PCM block must not be in use (dtparam=i2s off).
#pragma once

#include <cstdint>

#include "aow/indicators.h"

namespace aow::hat {

constexpr int kLedCount = 4;
constexpr int kLedWords = kLedCount * 3 + 1;  // one word per colour byte, then a zero word

// One colour byte as 32 PCM bits, most significant LED bit first.
constexpr uint32_t sk6812Byte(uint8_t v) {
    uint32_t w = 0;
    for (int bit = 7; bit >= 0; --bit) w = (w << 4) | ((v >> bit) & 1 ? 0xCu : 0x8u);
    return w;
}

// The frame for the chain, first LED first, in the SK6812's G, R, B order. The last word is
// zero so the line rests low whatever the PCM does once its FIFO is empty.
inline void sk6812Frame(const indicators::Rgb (&leds)[kLedCount], uint32_t (&out)[kLedWords]) {
    int n = 0;
    for (const auto& c : leds) {
        out[n++] = sk6812Byte(c.g);
        out[n++] = sk6812Byte(c.r);
        out[n++] = sk6812Byte(c.b);
    }
    out[n] = 0;
}

class HatLeds {
public:
    HatLeds() = default;
    ~HatLeds();
    HatLeds(const HatLeds&) = delete;
    HatLeds& operator=(const HatLeds&) = delete;

    // Maps the registers, starts the PCM clock, puts GPIO21 on PCM_DOUT and sends an all-off
    // frame. Returns false and logs why if any of it fails (the daemon runs on without LEDs).
    bool open();
    bool ok() const { return pcm_ != nullptr; }
    // Never blocks: 13 register writes.
    void show(const indicators::Rgb (&leds)[kLedCount]);

private:
    bool send(const uint32_t (&words)[kLedWords]);

    volatile uint32_t* gpio_ = nullptr;
    volatile uint32_t* pcm_ = nullptr;
    volatile uint32_t* clk_ = nullptr;
};

}  // namespace aow::hat
