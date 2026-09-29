// The daemon's side of the PGN conversation with the app. Answers as the steer (126) and
// GPS (120) modules: hello and scan replies, and a PGN 253 for every PGN 254.
// Keeps the latest PGN 254 and when it arrived; acting on it is the steering code's job.
#pragma once

#include <cstdint>

#include "aow/hal.h"
#include "aow/steer_pgn.h"

namespace aow {

class AppLink {
public:
    AppLink(hal::UdpLink& udp, hal::Clock& clock, const uint8_t moduleIp[4]);

    // Drains every datagram waiting on the link and answers it. Call on each wakeup.
    void poll();

    // What the next PGN 253 reports. The steering code keeps this current.
    void setStatus(const pgn::SteerStatus& s) { status_ = s; }

    bool haveCommand() const { return haveCommand_; }
    const pgn::SteerCommand& command() const { return command_; }
    // Microseconds since the last valid PGN 254; UINT64_MAX if none has arrived.
    uint64_t usSinceCommand() const;

    struct Counters {
        uint32_t received = 0;   // valid frames from the app
        uint32_t rejected = 0;   // datagrams that failed parse or came from a module
        uint32_t commands = 0;   // PGN 254
        uint32_t sendErrors = 0;
    };
    const Counters& counters() const { return counters_; }

private:
    void handle(const uint8_t* buf, int len);
    void send(const uint8_t* frame, size_t len);

    hal::UdpLink& udp_;
    hal::Clock& clock_;
    uint8_t ip_[4];
    pgn::SteerStatus status_;
    pgn::SteerCommand command_;
    bool haveCommand_ = false;
    uint64_t commandAtUs_ = 0;
    Counters counters_;
};

}  // namespace aow
