#include "aow/helper_status.h"

#include <cstring>

#include "aow/pgn.h"
#include "check.h"

using namespace aow;
using namespace aow::helper;

int main() {
    LinkGateway s;
    s.clientConnected = true;
    s.allNetworks = true;
    s.port = 2323;
    s.monitors = 1;
    s.connections = 3;
    s.toClient = 24439;
    s.toBus = 314;
    s.txDropped = 2;
    s.rxDropped = 0x01020304;
    std::strcpy(s.client, "isoshim bench on macOS 26.6.2");
    std::strcpy(s.clientAddress, "192.168.4.227");
    s.channelCount = 2;
    std::strcpy(s.channels[0].interface, "hatcan2");
    s.channels[0].bitrate = 250000;
    s.channels[0].state = 0;
    std::strcpy(s.channels[1].interface, "hatcan3");
    s.channels[1].state = 3;
    s.channels[1].txErrors = 255;
    s.channels[1].rxErrors = 7;

    // A whole PGN frame, from 0x81, PGN 153, that the app link's parser takes.
    uint8_t buf[300];
    const size_t n = encode(s, buf, sizeof buf);
    CHECK(n > pgn::kOverhead);
    pgn::Frame f;
    CHECK(pgn::parse(buf, n, f) && f.src == kSrcLinkGateway && f.pgn == kPgnLinkGateway);

    LinkGateway d;
    CHECK(decode(f.data, f.len, d));
    CHECK(d.clientConnected && d.allNetworks && d.port == 2323 && d.monitors == 1 && d.connections == 3);
    CHECK(d.toClient == 24439 && d.toBus == 314 && d.txDropped == 2 && d.rxDropped == 0x01020304);
    CHECK(!std::strcmp(d.client, "isoshim bench on macOS 26.6.2") && !std::strcmp(d.clientAddress, "192.168.4.227"));
    CHECK(d.channelCount == 2 && !std::strcmp(d.channels[0].interface, "hatcan2") && d.channels[0].bitrate == 250000);
    CHECK(d.channels[1].state == 3 && d.channels[1].txErrors == 255 && d.channels[1].rxErrors == 7);
    CHECK(!std::strcmp(busStateName(d.channels[1].state), "bus-off") && !std::strcmp(busStateName(9), "down"));

    // Short, another format, too small an output: refused, and d untouched.
    CHECK(!decode(f.data, f.len - 1, d) && d.port == 2323);
    uint8_t other[255];
    std::memcpy(other, f.data, f.len);
    other[0] = 2;
    CHECK(!decode(other, f.len, d));
    CHECK(encode(s, buf, 20) == 0);

    // A name too long for the field is cut, not overrun.
    LinkGateway lng;
    std::memset(lng.client, 'x', sizeof lng.client - 1);
    const size_t m = encode(lng, buf, sizeof buf);
    CHECK(pgn::parse(buf, m, f) && decode(f.data, f.len, d) && std::strlen(d.client) == sizeof d.client - 1);

    // Nothing connected: empty strings, zero channels.
    const size_t e = encode(LinkGateway{}, buf, sizeof buf);
    CHECK(pgn::parse(buf, e, f) && decode(f.data, f.len, d) && !d.clientConnected && d.client[0] == 0 && d.channelCount == 0);

    return finish("helper_status");
}
