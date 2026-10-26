// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
//
// Watches the HAT's CAN channels for one that has stopped sending for no reason the bus explains
// (aow/can_stall.h) and takes its interface down and up. Its own normal-priority thread: it runs
// "ip" and "tc", which no other thread should wait for.
#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "shared.h"

namespace aow {

class CanWatch {
public:
    explicit CanWatch(Shared& shared) : shared_(shared) {}
    ~CanWatch() { stop(); }

    // A channel to watch: 0..2 for CAN1..CAN3, its interface and the bit rate to bring it back at.
    void add(int index, const std::string& interface, uint32_t bitrate);
    void start();
    void stop();

private:
    struct Channel { int index; std::string interface; uint32_t bitrate; };
    void run();

    Shared& shared_;
    std::vector<Channel> channels_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace aow
