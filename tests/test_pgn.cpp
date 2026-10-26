// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors

#include "aow/pgn.h"

#include <cstring>

#include "check.h"

using namespace aow;

int main() {
    // The host hello, byte for byte as AgOpenWeb and AgIO send it (fixed crc 0x47).
    const uint8_t hello[] = {0x80, 0x81, 0x7F, 0xC8, 0x03, 0x38, 0x00, 0x00, 0x47};
    pgn::Frame f;
    CHECK(pgn::parse(hello, sizeof hello, f));
    CHECK(f.src == pgn::kSrcHost && f.pgn == pgn::Hello && f.len == 3);

    // Round trip.
    const uint8_t data[] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t buf[32];
    const size_t n = pgn::build(pgn::SrcSteer, pgn::FromSteer, data, sizeof data, buf, sizeof buf);
    CHECK(n == sizeof data + pgn::kOverhead);
    CHECK(buf[n - 1] == static_cast<uint8_t>(126 + 253 + 8 + 36));
    CHECK(pgn::parse(buf, n, f));
    CHECK(f.src == pgn::SrcSteer && f.pgn == pgn::FromSteer && f.len == 8);
    CHECK(std::memcmp(f.data, data, sizeof data) == 0);

    // Rejections.
    buf[n - 1] ^= 0xFF;
    CHECK(!pgn::parse(buf, n, f));           // bad crc
    buf[n - 1] ^= 0xFF;
    CHECK(!pgn::parse(buf, n - 1, f));       // truncated
    buf[0] = 0x00;
    CHECK(!pgn::parse(buf, n, f));           // bad header
    CHECK(pgn::build(1, 2, data, 8, buf, 8) == 0);  // does not fit

    // A steer-data frame with crc 0x47 that is not its real crc must be rejected.
    uint8_t bad[] = {0x80, 0x81, 0x7F, 0xFE, 0x01, 0x00, 0x47};
    CHECK(!pgn::parse(bad, sizeof bad, f));

    return finish("pgn");
}
