// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
//
// AgOpenGPS PGN framing over UDP.
//
// Frame: 0x80 0x81 <src> <pgn> <len> <data[len]> <crc>
// crc = low byte of the sum of bytes 2 .. n-2 (src, pgn, len, data).
// Hello (200), subnet (201) and scan (202) from the host carry a fixed crc of 0x47.
#pragma once

#include <cstddef>
#include <cstdint>

namespace aow::pgn {

inline constexpr uint8_t kHeader0 = 0x80;
inline constexpr uint8_t kHeader1 = 0x81;
inline constexpr uint8_t kSrcHost = 0x7F;
inline constexpr size_t kOverhead = 6;  // header(2) + src + pgn + len + crc
inline constexpr size_t kMaxData = 255;

enum Id : uint8_t {
    CorrectedPosition = 100,  // the app's corrected position, for NMEA out (AgOpenGPS's GPS_Out)
    Hello = 200,
    SubnetChange = 201,
    Scan = 202,
    ScanReply = 203,
    HardwareMessage = 221,
    Sections64 = 229,       // 64 section on/off bits (sent when more than 16 are configured)
    MachinePins = 236,
    MachineConfig = 238,
    MachineData = 239,
    SectionStatus = 240,    // AOG-TaskController's, to the app: section control on, count, bits
    SectionControl = 241,   // AOG-TaskController's: byte 0 = 1, ISOBUS section control on
    VehicleGeometryData = 247,  // AgOpenWeb's: antenna and IMU positions, wheelbase (#387)
    TurnSensor = 250,
    SteerConfig = 251,
    SteerSettings = 252,
    FromSteer = 253,
    SteerData = 254,
};

// Module source addresses, which double as the hello-reply PGN.
enum Src : uint8_t {
    SrcGps = 120,
    SrcImu = 121,
    SrcMachine = 123,
    SrcSteer = 126,
    SrcTaskController = 128,  // what AOG-TaskController sends PGN 240 as
};

struct Frame {
    uint8_t src = 0;
    uint8_t pgn = 0;
    uint8_t len = 0;
    const uint8_t* data = nullptr;  // points into the buffer passed to parse()
};

uint8_t crc(const uint8_t* frame, size_t frameLen);

// Writes a complete frame into out. Returns the frame length, or 0 if it does not fit.
size_t build(uint8_t src, uint8_t pgn, const uint8_t* data, uint8_t len,
             uint8_t* out, size_t outCap);

// Validates header, length and crc. On success fills f and returns true.
bool parse(const uint8_t* buf, size_t bufLen, Frame& f);

// PGN 100: the position AgOpenGPS calls pn.fix, the antenna with its left/right offset, roll and
// drift applied (not moved back to the pivot), sent once per fix. Doubles, little-endian:
// longitude, latitude, then the vehicle's heading (degrees true). Older AgOpenGPS sends 16
// bytes, with no heading.
struct CorrectedFix {
    double lat = 0, lon = 0, heading = 0;
    bool hasHeading = false;
};
// False for a short frame or a position that is not one.
bool decodeCorrectedFix(const uint8_t* data, size_t len, CorrectedFix& f);

// PGN 247, AgOpenWeb's vehicle geometry (its Docs/PGN.md, "Vehicle geometry"): sent with PGN
// 251/252, when a vehicle setting changes, and when PGN 253 is heard again after a silence.
// Lengths in cm. Positions from the pivot (rear axle centre): pivot ahead (+), offset right (+),
// height above the ground. Layout version 1 is 19 bytes; later versions only add fields at the
// end, so a longer frame is read for the fields known here.
struct VehicleGeometry {
    uint8_t version = 0;
    bool imuEntered = false;  // the IMU position was set (else the IMU fields are 0, not a position)
    uint16_t wheelbaseCm = 0, trackWidthCm = 0, antennaHeightCm = 0;
    int16_t antennaPivotCm = 0, antennaOffsetCm = 0;
    int16_t imuPivotCm = 0;
    uint16_t imuHeightCm = 0;
    int16_t imuOffsetCm = 0;
    uint8_t maxSteerDeg = 0;
};
// False for a short frame or layout version 0.
bool decodeVehicleGeometry(const uint8_t* data, size_t len, VehicleGeometry& g);

}  // namespace aow::pgn
