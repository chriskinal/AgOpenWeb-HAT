// The web thread: pages and API on HTTP, telemetry and log WebSockets, and the log outputs
// (journal, UDP syslog). Runs at normal priority and never touches hardware.
#pragma once

#include <cstdint>
#include <thread>

#include "shared.h"

namespace aow {

struct WebOptions {
    uint16_t httpPort = 80;  // 0: no pages, but logs still go out
    uint16_t gvretPort = 23;  // SavvyCAN; 0: off
    const char* version = "";
    SettingsFile settingsFile;
};

class WebThread {
public:
    WebThread(Shared& shared, WebOptions opt) : shared_(shared), opt_(std::move(opt)) {}
    ~WebThread() { join(); }

    void start() { thread_ = std::thread([this] { run(); }); }
    void join() { if (thread_.joinable()) thread_.join(); }

private:
    void run();

    Shared& shared_;
    WebOptions opt_;
    std::thread thread_;
};

}  // namespace aow
