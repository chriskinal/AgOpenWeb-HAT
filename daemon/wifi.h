// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
//
// The CM4's Wi-Fi through NetworkManager (nmcli): client or the cab's access point
// (core/include/aow/wifi.h). nmcli takes tens of milliseconds a call, so it runs on a thread of
// its own, never on the web thread: that thread keeps the state the page reads, carries out a
// switch (after the page has had its reply, since a switch takes the page's own connection away,
// and back to the connection the unit had if it fails), and the fallback after boot.
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "aow/wifi.h"

namespace aow {

class WifiControl {
public:
    WifiControl();
    ~WifiControl();

    wifi::State state();
    std::vector<wifi::Seen> seen();  // the last scan's networks (client mode)
    // Checks the request and queues the switch; false with the reason if it cannot. With
    // activate false only the access point's profile is written (client mode: for the fallback).
    bool apply(const wifi::Request& r, bool activate, std::string& error);
    bool setFallback(bool on, std::string& error);

private:
    struct Profile {
        std::string uuid, name, ssid, mode, band;
        int channel = 0;
        bool autoconnect = false;
    };
    struct Job { wifi::Request request; bool activate = true; };

    void run();
    void refresh();
    std::vector<Profile> profiles();
    void carryOut(const Job& job);
    void watchFallback();
    void finish(const char* job, const std::string& message);

    std::mutex mutex_;  // everything below; shared by the web thread and this one's
    std::condition_variable wake_;
    wifi::State state_;
    std::vector<wifi::Seen> seen_;
    std::optional<Job> pending_;
    bool stopping_ = false;
    bool fallbackDone_ = false;
    std::thread thread_;
};

}  // namespace aow
