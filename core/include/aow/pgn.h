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

}  // namespace aow::pgn
