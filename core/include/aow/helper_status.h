// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
//
// What the helper services tell aow-hatd about themselves (docs/PLAN.md section 8, "the link"):
// datagrams over UDP to 127.0.0.1:8891, once a second. aow-hatd only shows them, on /api/status
// and its pages, and never acts on them; a helper not heard from for 3 s shows as not running.
//
// The AOW link gateway (aow-linkd, SharpIsoTerm's gateway/linux; docs/PLAN_VT.md section 2) sends
// its --status report: one JSON object, "gateway":"aow-linkd" (SharpIsoTerm gateway/README.md).
// The task controller's own status (aow-tcd) will come on the same port.
#pragma once

#include <cstddef>
#include <cstdint>

namespace aow::helper {

constexpr uint16_t kPort = 8891;
constexpr uint32_t kStaleMs = 3000;

struct LinkGateway {
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

// aow-linkd's report. False if it is not one (not JSON, or another helper's); strings too long
// for their field are cut, channels past kMaxChannels left out.
bool decodeLinkGateway(const char* json, size_t len, LinkGateway& s);

const char* busStateName(uint8_t state);  // "error active" .. "bus-off", "down"

}  // namespace aow::helper
