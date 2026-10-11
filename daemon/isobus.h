// ISOBUS on one of the HAT's CAN channels (docs/PLAN.md section 8), on AgIsoStack++.
//
// The stack runs on its own normal-priority threads and opens its channel itself. The control
// thread never calls into it and never waits on its locks: what it needs crosses through Shared.
//
// It claims an address and runs AOG-TaskController's section control task controller
// (third_party/aog-taskcontroller): the app's section bits go to the implement as setpoints, and
// what the implement reports comes back in Shared::isobus().
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
    // directory: where implements' device descriptions and tc.log are kept.
    void start(const std::string& interface, uint8_t channel, const std::string& directory);
    void stop();

    // The identity number in our NAME: 21 bits from this unit's machine id, so two HATs on
    // one bus do not claim with the same NAME.
    static uint32_t identityNumber();

private:
    void run();
    void publish();
    uint64_t framesSent() const;  // the interface's count of acknowledged transmissions

    Shared& shared_;
    std::string interface_;
    std::string directory_;
    uint64_t sentAtStart_ = 0;
    IsobusStatus status_;  // the ISOBUS thread's copy; publish() hands it to Shared
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace aow
