#include "aow/helper_status.h"

#include <cstring>

#include "aow/pgn.h"

namespace aow::helper {

namespace {

// Layout of the PGN 153 body, little-endian:
//   format u8 (1), flags u8 (bit 0 client connected, bit 1 all networks), port u16, monitors u8,
//   connections u32, toClient u32, toBus u32, txDropped u32, rxDropped u32,
//   client str, clientAddress str, channels u8, then per channel
//   interface str, bitrate u32, state u8, txErrors u8, rxErrors u8.
// str: length u8 and that many bytes, cut to fit the field it is read into.

struct Writer {
    uint8_t* p;
    size_t cap, n = 0;
    bool ok = true;
    void u8(uint8_t v) {
        if (n < cap) p[n++] = v;
        else ok = false;
    }
    void u16(uint16_t v) { u8(v & 0xFF); u8(v >> 8); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) u8((v >> (8 * i)) & 0xFF); }
    void str(const char* s, size_t max) {
        const size_t len = strnlen(s, max);
        u8(static_cast<uint8_t>(len));
        for (size_t i = 0; i < len; ++i) u8(static_cast<uint8_t>(s[i]));
    }
};

struct Reader {
    const uint8_t* p;
    size_t len, n = 0;
    bool ok = true;
    uint8_t u8() {
        if (n < len) return p[n++];
        ok = false;
        return 0;
    }
    uint16_t u16() { const uint16_t lo = u8(); return static_cast<uint16_t>(lo | (u8() << 8)); }
    uint32_t u32() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(u8()) << (8 * i);
        return v;
    }
    void str(char* out, size_t size) {
        const size_t l = u8();
        size_t i = 0;
        for (; i < l; ++i) {
            const char c = static_cast<char>(u8());
            if (i + 1 < size) out[i] = c;
        }
        out[i + 1 < size ? i : size - 1] = '\0';
    }
};

}  // namespace

size_t encode(const LinkGateway& s, uint8_t* out, size_t cap) {
    uint8_t body[pgn::kMaxData];
    Writer w{body, sizeof body};
    w.u8(LinkGateway::kFormat);
    w.u8(static_cast<uint8_t>((s.clientConnected ? 1 : 0) | (s.allNetworks ? 2 : 0)));
    w.u16(s.port);
    w.u8(s.monitors);
    w.u32(s.connections);
    w.u32(s.toClient);
    w.u32(s.toBus);
    w.u32(s.txDropped);
    w.u32(s.rxDropped);
    w.str(s.client, sizeof s.client - 1);
    w.str(s.clientAddress, sizeof s.clientAddress - 1);
    const uint8_t count = s.channelCount > LinkGateway::kMaxChannels ? LinkGateway::kMaxChannels : s.channelCount;
    w.u8(count);
    for (uint8_t i = 0; i < count; ++i) {
        const LinkGateway::Channel& c = s.channels[i];
        w.str(c.interface, sizeof c.interface - 1);
        w.u32(c.bitrate);
        w.u8(c.state);
        w.u8(c.txErrors);
        w.u8(c.rxErrors);
    }
    if (!w.ok) return 0;
    return pgn::build(kSrcLinkGateway, kPgnLinkGateway, body, static_cast<uint8_t>(w.n), out, cap);
}

bool decode(const uint8_t* data, size_t len, LinkGateway& s) {
    Reader r{data, len};
    if (r.u8() != LinkGateway::kFormat) return false;
    LinkGateway d;
    const uint8_t flags = r.u8();
    d.clientConnected = flags & 1;
    d.allNetworks = flags & 2;
    d.port = r.u16();
    d.monitors = r.u8();
    d.connections = r.u32();
    d.toClient = r.u32();
    d.toBus = r.u32();
    d.txDropped = r.u32();
    d.rxDropped = r.u32();
    r.str(d.client, sizeof d.client);
    r.str(d.clientAddress, sizeof d.clientAddress);
    const uint8_t count = r.u8();
    for (uint8_t i = 0; i < count; ++i) {
        LinkGateway::Channel c;
        r.str(c.interface, sizeof c.interface);
        c.bitrate = r.u32();
        c.state = r.u8();
        c.txErrors = r.u8();
        c.rxErrors = r.u8();
        if (i < LinkGateway::kMaxChannels) d.channels[d.channelCount++] = c;
    }
    if (!r.ok) return false;
    s = d;
    return true;
}

const char* busStateName(uint8_t state) {
    switch (state) {
    case 0: return "error active";
    case 1: return "error warning";
    case 2: return "error passive";
    case 3: return "bus-off";
    default: return "down";
    }
}

}  // namespace aow::helper
