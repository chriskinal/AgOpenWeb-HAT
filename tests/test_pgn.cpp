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

    // PGN 247, vehicle geometry, laid out as AgOpenWeb's PgnBuilder.BuildVehicleGeometryPgn:
    // wheelbase 2.50 m, track 1.80, antenna 3.00 high, 1.20 ahead, 0.15 left; IMU 0.50 behind,
    // 0.80 high, 0.25 right; max steer 45 degrees.
    {
        uint8_t data[21] = {1, 0x01, 250, 0, 180, 0, 0x2C, 0x01, 120, 0, 0xF1, 0xFF, 0xCE, 0xFF, 80, 0, 25, 0, 45, 0xAA, 0xBB};
        uint8_t frame[40];
        const size_t n = pgn::build(pgn::kSrcHost, pgn::VehicleGeometryData, data, 19, frame, sizeof frame);
        pgn::Frame gf;
        CHECK(n == 25 && pgn::parse(frame, n, gf) && gf.pgn == 247 && gf.len == 19);
        pgn::VehicleGeometry g;
        CHECK(pgn::decodeVehicleGeometry(gf.data, gf.len, g));
        CHECK(g.version == 1 && g.imuEntered && g.wheelbaseCm == 250 && g.trackWidthCm == 180);
        CHECK(g.antennaHeightCm == 300 && g.antennaPivotCm == 120 && g.antennaOffsetCm == -15);
        CHECK(g.imuPivotCm == -50 && g.imuHeightCm == 80 && g.imuOffsetCm == 25 && g.maxSteerDeg == 45);
        // A later layout version, longer: the known fields are read, the rest skipped.
        data[0] = 2;
        CHECK(pgn::decodeVehicleGeometry(data, 21, g) && g.version == 2 && g.imuOffsetCm == 25);
        // No IMU position entered.
        data[1] = 0;
        CHECK(pgn::decodeVehicleGeometry(data, 19, g) && !g.imuEntered);
        // Too short, or version 0: refused, and g left as it was.
        CHECK(!pgn::decodeVehicleGeometry(data, 18, g) && g.version == 2);
        data[0] = 0;
        CHECK(!pgn::decodeVehicleGeometry(data, 19, g));
    }

    return finish("pgn");
}
