// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors

#include "aow/pgn.h"

#include <cmath>
#include <cstring>

namespace aow::pgn {

uint8_t crc(const uint8_t* frame, size_t frameLen) {
    uint8_t sum = 0;
    for (size_t i = 2; i + 1 < frameLen; ++i) sum = static_cast<uint8_t>(sum + frame[i]);
    return sum;
}

size_t build(uint8_t src, uint8_t pgn, const uint8_t* data, uint8_t len,
             uint8_t* out, size_t outCap) {
    const size_t total = kOverhead + len;
    if (outCap < total || (len && !data)) return 0;
    out[0] = kHeader0;
    out[1] = kHeader1;
    out[2] = src;
    out[3] = pgn;
    out[4] = len;
    if (len) std::memcpy(out + 5, data, len);
    out[total - 1] = crc(out, total);
    return total;
}

bool parse(const uint8_t* buf, size_t bufLen, Frame& f) {
    if (!buf || bufLen < kOverhead) return false;
    if (buf[0] != kHeader0 || buf[1] != kHeader1) return false;
    const uint8_t len = buf[4];
    const size_t total = kOverhead + len;
    if (bufLen < total) return false;

    const uint8_t got = buf[total - 1];
    const uint8_t pgn = buf[3];
    const bool fixedCrc = (pgn == Hello || pgn == SubnetChange || pgn == Scan) && got == 0x47;
    if (!fixedCrc && got != crc(buf, total)) return false;

    f.src = buf[2];
    f.pgn = pgn;
    f.len = len;
    f.data = buf + 5;
    return true;
}

static double readDouble(const uint8_t* p) {
    uint64_t bits = 0;
    for (int i = 7; i >= 0; --i) bits = bits << 8 | p[i];
    double d;
    std::memcpy(&d, &bits, sizeof d);
    return d;
}

bool decodeCorrectedFix(const uint8_t* data, size_t len, CorrectedFix& f) {
    if (!data || len < 16) return false;
    CorrectedFix n;
    n.lon = readDouble(data);
    n.lat = readDouble(data + 8);
    if (!std::isfinite(n.lat) || !std::isfinite(n.lon) || std::fabs(n.lat) > 90 || std::fabs(n.lon) > 180) return false;
    if (len >= 24) {
        n.heading = readDouble(data + 16);
        n.hasHeading = std::isfinite(n.heading);
        if (n.hasHeading) {
            n.heading = std::fmod(n.heading, 360.0);
            if (n.heading < 0) n.heading += 360.0;
        }
    }
    f = n;
    return true;
}

}  // namespace aow::pgn
