// GNSS receiver sentences to a navigation state. Ported from the Teensy firmware's
// GNSSProcessor: GGA, GNS, VTG, HPR, KSXT, and Unicore INSPVAA / INSPVAXA.
#pragma once

#include <cstdint>

namespace aow::gnss {

struct State {
    double latitude = 0;          // decimal degrees, south negative
    double longitude = 0;         // decimal degrees, west negative
    float altitude = 0;           // m
    double fixTime = 0;           // UTC as HHMMSS.ss
    uint16_t gpsWeek = 0;         // from INSPVA*; 0 = not known
    double gpsSeconds = 0;        // seconds of week

    uint8_t fixQuality = 0;       // GGA scheme: 0 none, 1 GPS, 2 DGPS, 4 RTK fixed, 5 RTK float
    uint8_t numSatellites = 0;
    float hdop = 0;
    float ageDgps = 0;            // s

    float speedKnots = 0;
    float headingTrue = 0;        // VTG track, deg

    float dualHeading = 0;        // deg: HPR / KSXT heading, or INS azimuth
    float dualRoll = 0;           // deg: HPR / KSXT pitch (AgOpenGPS uses it as roll), or INS roll
    uint8_t headingQuality = 0;

    float insRoll = 0, insPitch = 0, insHeading = 0;  // deg
    float northVel = 0, eastVel = 0, upVel = 0;       // m/s
    uint8_t insAlignment = 0;     // 0 inactive, 1 aligning, 2 high variance, 3 good, 6 free, 7 complete
    uint8_t posType = 0;          // Unicore position type code, INSPVAXA

    bool hasPosition = false;
    bool hasVelocity = false;
    bool hasDualHeading = false;
    bool hasIns = false;

    uint8_t messageMask = 0;      // bit per Message below that has been seen
    uint64_t lastUpdateUs = 0;    // last position-bearing sentence
};

enum class Message : uint8_t { None, Gga, Gns, Vtg, Hpr, Ksxt, Inspvaa, Inspvaxa };

// The messages that carry a position; each one is a cue to send PANDA or PAOGI.
bool carriesPosition(Message m);

class Parser {
public:
    // body: a checksum-verified sentence from nmea::SentenceReader ('$' or '#' first, no '*').
    // Returns the message it recognised and applied, or None.
    Message parse(const char* body, uint64_t nowUs);

    const State& state() const { return s_; }

private:
    State s_;
};

// GPS week + seconds of week to UTC HHMMSS.ss (18 leap seconds, as the firmware).
double gpsToUtc(uint16_t week, double seconds);

}  // namespace aow::gnss
