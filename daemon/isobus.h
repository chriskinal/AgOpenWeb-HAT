// ISOBUS on one of the HAT's CAN channels (docs/PLAN.md section 8), on AgIsoStack++.
//
// The stack runs on its own normal-priority threads and opens its channel itself. The control
// thread never calls into it and never waits on its locks: what it needs crosses through Shared.
// For now this claims an address as the task controller and nothing more.
#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "shared.h"

namespace aow {

class Isobus {
public:
    explicit Isobus(Shared& shared) : shared_(shared) {}
    ~Isobus() { stop(); }

    // Starts the stack on a SocketCAN interface that is already up at 250 kbit/s. Returns at
    // once; the address claim goes on in the background and shows in Shared::isobus().
    void start(const std::string& interface, uint8_t channel);
    void stop();

    // The identity number in our NAME: 21 bits from this unit's machine id, so two HATs on
    // one bus do not claim with the same NAME.
    static uint32_t identityNumber();

private:
    void run();
    void publish(IsobusStatus::State state, uint8_t address = 0xFE);
    uint64_t framesSent() const;  // the interface's count of acknowledged transmissions

    Shared& shared_;
    std::string interface_;
    uint8_t channel_ = 0;
    uint64_t name_ = 0;
    uint64_t sentAtStart_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace aow
