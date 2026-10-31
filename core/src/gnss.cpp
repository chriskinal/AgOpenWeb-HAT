// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
// Ported from Firmware_Teensy_AiO-New-Dawn, copyright 2025 by the AOG Group.

#include "aow/gnss.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "aow/nmea.h"

namespace aow::gnss {

namespace {

using nmea::Fields;

constexpr float kMsToKnots = 1.94384f;
constexpr float kKmhToKnots = 0.539957f;

// NMEA ddmm.mmmm (or dddmm.mmmm) plus hemisphere to signed decimal degrees.
double nmeaToDegrees(double v, char hemi) {
    const double deg = std::floor(v / 100.0);
    const double d = deg + (v - deg * 100.0) / 60.0;
    return (hemi == 'S' || hemi == 'W') ? -d : d;
}

// NMEA type after an optional two-letter talker: "GNGGA" -> "GGA". Proprietary names
// (KSXT, INSPVAA, INSPVAXA) are matched whole first.
Message identify(const char* name) {
    if (!std::strcmp(name, "KSXT") || !std::strcmp(name, "GPKSXT")) return Message::Ksxt;
    if (!std::strcmp(name, "INSPVAA")) return Message::Inspvaa;
    if (!std::strcmp(name, "INSPVAXA")) return Message::Inspvaxa;
    if (std::strlen(name) != 5) return Message::None;
    const char* t = name + 2;
    if (!std::strcmp(t, "GGA")) return Message::Gga;
    if (!std::strcmp(t, "GNS")) return Message::Gns;
    if (!std::strcmp(t, "VTG")) return Message::Vtg;
    if (!std::strcmp(t, "HPR")) return Message::Hpr;
    return Message::None;
}

void setBit(State& s, Message m) { s.messageMask |= static_cast<uint8_t>(1u << static_cast<int>(m)); }

// GGA and GNS share fields 1-9.
void applyFix(State& s, const Fields& f) {
    if (!f.empty(1)) s.fixTime = f.num(1);
    if (std::strlen(f[2]) >= 4 && !f.empty(3)) s.latitude = nmeaToDegrees(f.num(2), f[3][0]);
    else s.latitude = 0;
    if (std::strlen(f[4]) >= 5 && !f.empty(5)) s.longitude = nmeaToDegrees(f.num(4), f[5][0]);
    else s.longitude = 0;
    if (!f.empty(7)) s.numSatellites = static_cast<uint8_t>(f.integer(7));
    if (!f.empty(8)) s.hdop = static_cast<float>(f.num(8));
    if (!f.empty(9)) s.altitude = static_cast<float>(f.num(9));
}

bool parseGga(State& s, const Fields& f, uint64_t now) {
    if (f.count() < 9) return false;
    applyFix(s, f);
    if (!f.empty(6)) s.fixQuality = static_cast<uint8_t>(f[6][0] - '0');
    if (!f.empty(13)) s.ageDgps = static_cast<float>(f.num(13));
    s.hasPosition = (s.latitude != 0.0 || s.longitude != 0.0) && s.fixQuality >= 1;
    s.lastUpdateUs = now;
    return true;
}

bool parseGns(State& s, const Fields& f, uint64_t now) {
    if (f.count() < 9) return false;
    applyFix(s, f);
    if (!f.empty(6)) {
        switch (f[6][0]) {  // mode string, first system decides
        case 'A': s.fixQuality = 1; break;
        case 'D': s.fixQuality = 2; break;
        case 'P': s.fixQuality = 3; break;
        case 'R': s.fixQuality = 4; break;
        case 'F': s.fixQuality = 5; break;
        default: s.fixQuality = 0; break;
        }
    }
    // Differential age is field 11 (12 is the station ID; the firmware reads 12).
    if (!f.empty(11)) s.ageDgps = static_cast<float>(f.num(11));
    s.hasPosition = (s.latitude != 0.0 || s.longitude != 0.0) && s.fixQuality >= 1;
    s.lastUpdateUs = now;
    return true;
}

bool parseVtg(State& s, const Fields& f) {
    if (f.count() < 8) return false;
    if (!f.empty(1)) s.headingTrue = static_cast<float>(f.num(1));
    if (!f.empty(5)) {
        s.speedKnots = static_cast<float>(f.num(5));
        s.hasVelocity = true;
    }
    return true;  // no position: lastUpdateUs unchanged
}

// $GNHPR,time,heading,pitch,roll,quality,satellites,age,reserved
bool parseHpr(State& s, const Fields& f) {
    if (f.count() < 8) return false;
    if (!f.empty(1)) s.fixTime = f.num(1);
    if (!f.empty(2)) s.dualHeading = static_cast<float>(f.num(2));
    if (!f.empty(3)) s.dualRoll = static_cast<float>(f.num(3));  // pitch: AgOpenGPS's roll
    if (!f.empty(5)) s.headingQuality = static_cast<uint8_t>(f.integer(5));
    if (!f.empty(6)) s.numSatellites = static_cast<uint8_t>(f.integer(6));
    if (!f.empty(7)) s.ageDgps = static_cast<float>(f.num(7));
    s.hasDualHeading = true;
    return true;  // no position: lastUpdateUs unchanged
}

// $KSXT,YYYYMMDDHHMMSS.ss,lon,lat,alt,heading,pitch,track,speed_kmh,roll,quality,...,sats
bool parseKsxt(State& s, const Fields& f, uint64_t now) {
    if (f.count() < 10) return false;
    if (std::strlen(f[1]) >= 14) s.fixTime = std::strtod(f[1] + 8, nullptr);
    if (!f.empty(2)) s.longitude = f.num(2);
    if (!f.empty(3)) s.latitude = f.num(3);
    if (!f.empty(4)) s.altitude = static_cast<float>(f.num(4));
    if (!f.empty(5)) s.dualHeading = static_cast<float>(f.num(5));
    if (!f.empty(6)) s.dualRoll = static_cast<float>(f.num(6));  // pitch: AgOpenGPS's roll
    if (!f.empty(8)) s.speedKnots = static_cast<float>(f.num(8)) * kKmhToKnots;
    if (!f.empty(10)) {
        uint8_t q = static_cast<uint8_t>(f.integer(10));
        if (q == 2) q = 5;       // float
        else if (q == 3) q = 4;  // fixed
        s.fixQuality = q;
        s.headingQuality = q;
    }
    if (!f.empty(13)) s.numSatellites = static_cast<uint8_t>(f.integer(13));
    s.hdop = 0;  // not in KSXT
    s.hasDualHeading = true;
    s.hasPosition = true;  // the firmware sets this whatever the quality
    s.lastUpdateUs = now;
    return true;
}

// INS status string to (alignment code, fix quality).
void applyInsStatus(State& s, const char* st) {
    struct Map { const char* name; uint8_t align, quality; };
    static const Map kMap[] = {
        {"INS_INACTIVE", 0, 0},          {"INS_ALIGNING", 1, 1},
        {"INS_HIGH_VARIANCE", 2, 2},     {"INS_SOLUTION_GOOD", 3, 4},
        {"INS_SOLUTION_FREE", 6, 1},     {"INS_ALIGNMENT_COMPLETE", 7, 5},
    };
    for (const Map& m : kMap) {
        if (!std::strncmp(st, m.name, std::strlen(m.name))) {
            s.insAlignment = m.align;
            s.fixQuality = m.quality;
            return;
        }
    }
    s.insAlignment = 0;
    s.fixQuality = 1;
}

void applyInsTime(State& s, const Fields& f) {
    if (f.empty(5) || f.empty(6)) return;
    s.gpsWeek = static_cast<uint16_t>(f.integer(5));
    s.gpsSeconds = f.num(6);
    s.fixTime = gpsToUtc(s.gpsWeek, s.gpsSeconds);
}

void applyVelocity(State& s, const Fields& f, int first) {
    if (f.empty(first) || f.empty(first + 1) || f.empty(first + 2)) return;
    s.northVel = static_cast<float>(f.num(first));
    s.eastVel = static_cast<float>(f.num(first + 1));
    s.upVel = static_cast<float>(f.num(first + 2));
    s.speedKnots = std::sqrt(s.northVel * s.northVel + s.eastVel * s.eastVel) * kMsToKnots;
    s.hasVelocity = true;
}

void applyAttitude(State& s, const Fields& f, int first) {
    if (f.empty(first) || f.empty(first + 1) || f.empty(first + 2)) return;
    s.insRoll = static_cast<float>(f.num(first));
    s.insPitch = static_cast<float>(f.num(first + 1));
    s.insHeading = static_cast<float>(f.num(first + 2));
    s.dualHeading = s.insHeading;
    s.dualRoll = s.insRoll;
    s.hasDualHeading = true;
}

// #INSPVAA,<9 header fields>;week,seconds,lat,lon,height,vN,vE,vU,roll,pitch,azimuth,status
bool parseInspvaa(State& s, const Fields& f, uint64_t now) {
    if (f.count() < 22) return false;
    if (!f.empty(12)) {
        s.latitude = f.num(12);
        s.hasPosition = true;
    }
    if (!f.empty(13)) s.longitude = f.num(13);
    if (!f.empty(14)) s.altitude = static_cast<float>(f.num(14));
    applyVelocity(s, f, 15);
    applyAttitude(s, f, 18);
    applyInsStatus(s, f[21]);
    s.posType = 16;
    s.numSatellites = 12;  // not in the message; the firmware's stand-in values
    s.hdop = 0.9f;
    applyInsTime(s, f);
    s.hasIns = true;
    s.lastUpdateUs = now;
    return true;
}

// #INSPVAXA,<9 header fields>;ins_status,pos_type,lat,lon,height,undulation,vN,vE,vU,roll,pitch,
// azimuth,9 standard deviations,ext_sol_stat,time_since_update
// AOW HAT: the velocities are fields 16-18, after the undulation (UM981 commands reference,
// table 2-11, and its example line); the firmware reads them at 15-17, which takes the
// undulation as the north velocity.
bool parseInspvaxa(State& s, const Fields& f, uint64_t now) {
    if (f.count() < 29) return false;
    bool insValid = true;
    if (!f.empty(10)) {
        applyInsStatus(s, f[10]);
        insValid = !f.startsWith(10, "INS_INACTIVE");
    }
    if (!f.empty(11)) {
        struct Map { const char* name; uint8_t type, quality; };
        static const Map kMap[] = {
            {"INS_RTKFIXED", 56, 4}, {"INS_RTKFLOAT", 55, 5},
            {"INS_PSRDIFF", 54, 2},  {"INS_PSRSP", 53, 1},
        };
        s.posType = 0;
        if (insValid) s.fixQuality = 1;
        for (const Map& m : kMap) {
            if (f.startsWith(11, m.name)) {
                s.posType = m.type;
                s.fixQuality = m.quality;
                break;
            }
        }
    }
    if (!f.empty(12)) s.latitude = f.num(12);
    if (!f.empty(13)) s.longitude = f.num(13);
    s.hasPosition = insValid && (s.latitude != 0.0 || s.longitude != 0.0);
    if (!f.empty(14)) s.altitude = static_cast<float>(f.num(14));
    applyVelocity(s, f, 16);
    applyAttitude(s, f, 19);
    if (!f.empty(32)) s.ageDgps = static_cast<float>(f.integer(32));  // time since update
    s.numSatellites = 12;
    s.hdop = 0.9f;
    applyInsTime(s, f);
    s.hasIns = true;
    s.lastUpdateUs = now;
    return true;
}

}  // namespace

bool carriesPosition(Message m) {
    return m == Message::Gga || m == Message::Gns || m == Message::Ksxt ||
           m == Message::Inspvaa || m == Message::Inspvaxa;
}

double gpsToUtc(uint16_t week, double seconds) {
    constexpr int64_t kLeapSeconds = 18;
    constexpr int64_t kDay = 86400;
    const double whole = std::floor(seconds);
    int64_t t = static_cast<int64_t>(week) * 7 * kDay + static_cast<int64_t>(whole) - kLeapSeconds;
    t %= kDay;
    if (t < 0) t += kDay;
    const int64_t h = t / 3600, m = t % 3600 / 60, sec = t % 60;
    return static_cast<double>(h * 10000 + m * 100 + sec) + (seconds - whole);
}

Message Parser::parse(const char* body, uint64_t nowUs) {
    if (!body || (body[0] != '$' && body[0] != '#')) return Message::None;
    const Fields f(body + 1, std::strlen(body + 1));
    const Message m = identify(f[0]);

    bool ok = false;
    switch (m) {
    case Message::Gga: ok = parseGga(s_, f, nowUs); break;
    case Message::Gns: ok = parseGns(s_, f, nowUs); break;
    case Message::Vtg: ok = parseVtg(s_, f); break;
    case Message::Hpr: ok = parseHpr(s_, f); break;
    case Message::Ksxt: ok = parseKsxt(s_, f, nowUs); break;
    case Message::Inspvaa: ok = parseInspvaa(s_, f, nowUs); break;
    case Message::Inspvaxa: ok = parseInspvaxa(s_, f, nowUs); break;
    case Message::None: break;
    }
    if (!ok) return Message::None;
    setBit(s_, m);
    return m;
}

}  // namespace aow::gnss
