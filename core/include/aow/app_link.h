// The daemon's side of the PGN conversation with the app. Answers as the steer (126) and
// GPS (120) modules: hello and scan replies, and a PGN 253 for every PGN 254.
// Keeps the latest PGN 254 and when it arrived; acting on it is the steering code's job.
#pragma once

#include <cstdint>

#include "aow/hal.h"
#include "aow/steer_pgn.h"

namespace aow {

// Whatever steers: gets the app's steering PGNs and says what PGN 253 reports.
struct SteerModule {
    virtual ~SteerModule() = default;
    virtual void onSteerData(const pgn::SteerCommand& c) = 0;
    virtual void onSteerConfig(const uint8_t* data, size_t len) = 0;    // PGN 251
    virtual void onSteerSettings(const uint8_t* data, size_t len) = 0;  // PGN 252
    virtual pgn::SteerStatus steerStatus() = 0;
};

class AppLink {
public:
    AppLink(hal::UdpLink& udp, hal::Clock& clock, const uint8_t moduleIp[4]);

    // Drains every datagram waiting on the link and answers it. Call on each wakeup.
    void poll();

    // Steering PGNs go to the module; without one, PGN 253 reports setStatus()'s value.
    void setSteerModule(SteerModule* m) { steer_ = m; }
    void setStatus(const pgn::SteerStatus& s) { status_ = s; }

    // PGN 250, sent by the caller at 10 Hz.
    void sendTurnSensor(uint8_t value);

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
    SteerModule* steer_ = nullptr;
    pgn::SteerStatus status_;
    pgn::SteerCommand command_;
    bool haveCommand_ = false;
    uint64_t commandAtUs_ = 0;
    Counters counters_;
};

}  // namespace aow
