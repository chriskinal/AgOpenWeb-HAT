// What the helper services tell aow-hatd about themselves (docs/PLAN.md section 8, "the link"):
// PGN frames (aow/pgn.h) over UDP to 127.0.0.1:8891, once a second. aow-hatd only shows them,
// on /api/status and its pages, and never acts on them; a helper not heard from for 3 s shows as
// not running.
//
// PGN 153, the AOW link gateway (aow-linkd, docs/PLAN_VT.md section 2). The task controller's
// own status (aow-tcd, provisional PGN 151) will come on the same port.
#pragma once

#include <cstddef>
#include <cstdint>

namespace aow::helper {

constexpr uint16_t kPort = 8891;
constexpr uint8_t kSrcLinkGateway = 0x81;
constexpr uint8_t kPgnLinkGateway = 153;  // free in AgOpenGPS's PGN list and AgOpenWeb (2026-10-25)
constexpr uint32_t kStaleMs = 3000;

struct LinkGateway {
    static constexpr uint8_t kFormat = 1;  // the body's first byte; another format is ignored
    static constexpr int kMaxChannels = 4;

    bool clientConnected = false;  // a control client (the terminal) is connected
    bool allNetworks = false;      // listening beyond loopback
    uint16_t port = 0;
    uint8_t monitors = 0;
    uint32_t connections = 0;      // control clients taken since the gateway started
    // The connected control client's, since it connected (0 with none).
    uint32_t toClient = 0, toBus = 0, txDropped = 0, rxDropped = 0;
    char client[48] = {};          // its name, from its HELLO
    char clientAddress[16] = {};

    struct Channel {
        char interface[16] = {};
        uint32_t bitrate = 0;
        uint8_t state = 4;  // the AOW link's BusState: 0 active .. 3 bus-off, 4 down
        uint8_t txErrors = 0, rxErrors = 0;
    };
    uint8_t channelCount = 0;
    Channel channels[kMaxChannels];
};

// The whole PGN frame into out. Returns its length, 0 if it does not fit.
size_t encode(const LinkGateway& s, uint8_t* out, size_t cap);
// A PGN 153 body. False if it is short or of another format.
bool decode(const uint8_t* data, size_t len, LinkGateway& s);

const char* busStateName(uint8_t state);  // "error active" .. "bus-off", "down"

}  // namespace aow::helper
