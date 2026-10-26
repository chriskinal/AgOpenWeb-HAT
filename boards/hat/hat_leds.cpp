// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
//
// See hat_leds.h.
#include "hat_leds.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "aow/log.h"
#include "hat_pins.h"

namespace aow::hat {

namespace {

constexpr uint32_t kGpioOffset = 0x200000, kPcmOffset = 0x203000, kClockOffset = 0x101000;
constexpr size_t kPage = 4096;

// PCM registers (word index)
constexpr int kCs = 0, kFifo = 1, kMode = 2, kTxc = 4;
constexpr uint32_t kCsEn = 1u << 0, kCsTxOn = 1u << 2, kCsTxClr = 1u << 3, kCsTxEmpty = 1u << 21;
constexpr uint32_t kMode32BitFrames = (31u << 10) | 1u;                    // FLEN 32 clocks, FSLEN 1
constexpr uint32_t kTxcCh1_32Bit = (1u << 31) | (1u << 30) | (8u << 16);  // width extension + 8, position 0

// Clock manager: the PCM clock's control and divider (word index), password in the top byte.
constexpr int kCmPcmCtl = 0x98 / 4, kCmPcmDiv = 0x9c / 4;
constexpr uint32_t kCmPassword = 0x5au << 24, kCmKill = 1u << 5, kCmEnable = 1u << 4, kCmBusy = 1u << 7;
constexpr uint32_t kCmSourceOsc = 1;
constexpr uint32_t kOscHz = 54'000'000;  // BCM2711
constexpr uint32_t kPcmHz = 3'000'000;

void sleepUs(long us) {
    timespec t{0, us * 1000};
    nanosleep(&t, nullptr);
}

// The SoC's peripheral base as the CPU sees it, from the device tree (0xFE000000 on a CM4).
uint32_t peripheralBase() {
    uint8_t b[16] = {};
    const int fd = ::open("/proc/device-tree/soc/ranges", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        const ssize_t n = ::read(fd, b, sizeof b);
        close(fd);
        if (n >= 12) {
            auto be32 = [&b](int i) { return uint32_t{b[i]} << 24 | uint32_t{b[i + 1]} << 16 | uint32_t{b[i + 2]} << 8 | b[i + 3]; };
            const uint32_t a = be32(4);
            return a ? a : be32(8);
        }
    }
    return 0xFE000000;
}

volatile uint32_t* mapPage(int fd, uint32_t address) {
    void* p = mmap(nullptr, kPage, PROT_READ | PROT_WRITE, MAP_SHARED, fd, address);
    return p == MAP_FAILED ? nullptr : static_cast<volatile uint32_t*>(p);
}

}  // namespace

HatLeds::~HatLeds() {
    if (pcm_) {
        const indicators::Rgb off[kLedCount] = {};
        show(off);
        sleepUs(1000);  // let the frame leave before the mapping goes
        munmap(const_cast<uint32_t*>(pcm_), kPage);
    }
    if (clk_) munmap(const_cast<uint32_t*>(clk_), kPage);
    if (gpio_) munmap(const_cast<uint32_t*>(gpio_), kPage);
}

bool HatLeds::open() {
    const int fd = ::open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        LOG_ERROR(System, "HAT LEDs: /dev/mem: %s", std::strerror(errno));
        return false;
    }
    const uint32_t base = peripheralBase();
    gpio_ = mapPage(fd, base + kGpioOffset);
    volatile uint32_t* pcm = mapPage(fd, base + kPcmOffset);
    clk_ = mapPage(fd, base + kClockOffset);
    close(fd);
    if (!gpio_ || !pcm || !clk_) {
        LOG_ERROR(System, "HAT LEDs: mapping the PCM registers: %s", std::strerror(errno));
        return false;
    }

    // PCM clock: stop, set oscillator / 18 = 3 MHz, start.
    pcm[kCs] = 0;
    clk_[kCmPcmCtl] = kCmPassword | kCmKill;
    sleepUs(20);
    for (int i = 0; i < 1000 && (clk_[kCmPcmCtl] & kCmBusy); ++i) sleepUs(10);
    clk_[kCmPcmDiv] = kCmPassword | ((kOscHz / kPcmHz) << 12);
    clk_[kCmPcmCtl] = kCmPassword | kCmSourceOsc;
    clk_[kCmPcmCtl] = kCmPassword | kCmSourceOsc | kCmEnable;
    sleepUs(20);
    for (int i = 0; i < 1000 && !(clk_[kCmPcmCtl] & kCmBusy); ++i) sleepUs(10);
    if (!(clk_[kCmPcmCtl] & kCmBusy)) {
        LOG_ERROR(System, "HAT LEDs: the PCM clock did not start");
        return false;
    }

    // PCM: one 32-bit channel, transmit only, fed from the FIFO by hand.
    pcm[kCs] = kCsEn;
    pcm[kMode] = kMode32BitFrames;
    pcm[kTxc] = kTxcCh1_32Bit;

    // GPIO21 to Alt0 = PCM_DOUT (function select 2, bits 3-5 = 100).
    const int shift = (kLedData % 10) * 3;
    gpio_[kLedData / 10] = (gpio_[kLedData / 10] & ~(7u << shift)) | (4u << shift);

    // All off, and check the frame really leaves: the FIFO must drain within a few ms.
    pcm_ = pcm;
    uint32_t frame[kLedWords];
    const indicators::Rgb off[kLedCount] = {};
    sk6812Frame(off, frame);
    send(frame);
    bool drained = false;
    for (int i = 0; i < 50 && !drained; ++i) {
        sleepUs(100);
        drained = pcm_[kCs] & kCsTxEmpty;
    }
    if (!drained) {
        LOG_ERROR(System, "HAT LEDs: the PCM did not shift the frame out (is I2S enabled?)");
        pcm_ = nullptr;
        munmap(const_cast<uint32_t*>(pcm), kPage);
        return false;
    }
    LOG_INFO(System, "HAT: %d status LEDs on GPIO%d (PCM, %u Hz bit clock)", kLedCount, kLedData, kOscHz / (kOscHz / kPcmHz));
    return true;
}

bool HatLeds::send(const uint32_t (&words)[kLedWords]) {
    pcm_[kCs] = kCsEn;             // transmitter off (the last frame left long ago)
    pcm_[kCs] = kCsEn | kCsTxClr;  // empty the FIFO; takes two PCM clocks
    for (int i = 0; i < 2000 && !(pcm_[kCs] & kCsTxEmpty); ++i) {}
    for (uint32_t w : words) pcm_[kFifo] = w;
    pcm_[kCs] = kCsEn | kCsTxOn;
    return true;
}

void HatLeds::show(const indicators::Rgb (&leds)[kLedCount]) {
    if (!pcm_) return;
    uint32_t frame[kLedWords];
    sk6812Frame(leds, frame);
    send(frame);
}

}  // namespace aow::hat
