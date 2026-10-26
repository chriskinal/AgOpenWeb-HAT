// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
//
// The HAT's piezo (BZ1, through Q10) on GPIO23: a square wave toggled by a background thread,
// so the control thread only hands over the tone list.
#pragma once

#include <atomic>
#include <thread>
#include <vector>

#include "aow/indicators.h"
#include "aow/pi_mutex.h"

namespace aow::hat {

class HatPiezo {
public:
    HatPiezo() = default;
    ~HatPiezo();
    HatPiezo(const HatPiezo&) = delete;
    HatPiezo& operator=(const HatPiezo&) = delete;

    // chipFd: an open GPIO character device. Returns false and logs why on failure.
    bool open(int chipFd, unsigned gpio);
    // Replaces whatever is playing. Does not block.
    void play(const std::vector<indicators::Tone>& tones);

private:
    void run();
    void set(bool high);
    bool sleepUntil(const timespec& t, uint32_t generation);  // false: interrupted

    int lineFd_ = -1;
    int wakeFd_ = -1;
    PiMutex mu_;  // play() is called from the control thread
    std::vector<indicators::Tone> pending_;
    std::atomic<uint32_t> generation_{0};
    std::atomic<bool> quit_{false};
    std::thread thread_;
};

}  // namespace aow::hat
