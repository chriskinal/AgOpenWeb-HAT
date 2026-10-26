// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
// Ported from Firmware_Teensy_AiO-New-Dawn, copyright 2025 by the AOG Group.

#include "aow/nav.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "aow/nmea.h"

namespace aow::nav {

namespace {

constexpr uint64_t kPositionMaxAgeUs = 5'000'000;

// Appends to a fixed buffer; any overflow makes the whole build fail.
struct Writer {
    char* out;
    size_t cap;
    size_t len = 0;
    bool ok = true;

    void add(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (!ok) return;
        va_list ap;
        va_start(ap, fmt);
        const int n = std::vsnprintf(out + len, cap - len, fmt, ap);
        va_end(ap);
        if (n < 0 || static_cast<size_t>(n) >= cap - len) ok = false;
        else len += static_cast<size_t>(n);
    }
};

// Signed decimal degrees to NMEA (d)ddmm.mmmmmm with a hemisphere letter. Rounds in whole
// micro-minutes so 59.9999999' carries into the degrees instead of printing 60.
void addCoordinate(Writer& w, double deg, int degDigits, char pos, char neg) {
    const auto micro = static_cast<int64_t>(std::llround(std::fabs(deg) * 60e6));
    const int64_t d = micro / 60'000'000;
    const int64_t rem = micro % 60'000'000;
    w.add("%0*lld%02lld.%06lld,%c", degDigits, static_cast<long long>(d),
          static_cast<long long>(rem / 1'000'000), static_cast<long long>(rem % 1'000'000),
          deg < 0 ? neg : pos);
}

// Fields 1-11, shared by both sentences.
void addCommon(Writer& w, const gnss::State& s, const char* name) {
    w.add("$%s,%08.1f,", name, s.fixTime);
    addCoordinate(w, s.latitude, 2, 'N', 'S');
    w.add(",");
    addCoordinate(w, s.longitude, 3, 'E', 'W');
    w.add(",%u,%u,%.1f,%.3f,%.1f,%.3f,", s.fixQuality, s.numSatellites, s.hdop, s.altitude,
          s.ageDgps, s.speedKnots);
}

size_t finish(Writer& w) {
    if (!w.ok) return 0;
    const uint8_t cs = nmea::xorChecksum(w.out + 1, w.len - 1);
    w.add("*%02X\r\n", cs);
    return w.ok ? w.len : 0;
}

}  // namespace

Sentence select(const gnss::State& s, uint64_t nowUs) {
    if (s.hasDualHeading || s.hasIns) return Sentence::Paogi;
    const bool fix = s.hasPosition && s.fixQuality > 0;
    const bool fresh = s.messageMask != 0 && nowUs - s.lastUpdateUs < kPositionMaxAgeUs;
    return fix && fresh ? Sentence::Panda : Sentence::None;
}

size_t buildPanda(const gnss::State& s, const imu::Data* imu, char* out, size_t cap) {
    Writer w{out, cap};
    addCommon(w, s, "PANDA");
    if (imu)  // heading x10, roll x10, pitch in degrees, yaw rate in deg/s
        w.add("%d,%d,%d,%.2f", static_cast<int>(imu->heading * 10.0f), static_cast<int>(std::lround(imu->roll * 10.0f)),
              static_cast<int>(std::lround(imu->pitch)), static_cast<double>(imu->yawRate));
    else
        w.add("65535,0,0,0");  // IMU heading (x10, 65535 = none), roll, pitch, yaw rate
    return finish(w);
}

size_t buildPaogi(const gnss::State& s, const imu::Data* imu, char* out, size_t cap) {
    if (!s.hasDualHeading) return 0;
    Writer w{out, cap};
    addCommon(w, s, "PAOGI");
    // Pitch from the INS if there is one, else the IMU's; the yaw rate only from an IMU.
    const float pitch = s.hasIns ? s.insPitch : imu ? imu->pitch : 0.0f;
    w.add("%.1f,%.2f,%d,%.2f", s.dualHeading, s.dualRoll, static_cast<int>(std::lround(pitch)),
          imu && !s.hasIns ? static_cast<double>(imu->yawRate) : 0.0);
    return finish(w);
}

size_t build(const gnss::State& s, const imu::Data* imu, uint64_t nowUs, char* out, size_t cap) {
    switch (select(s, nowUs)) {
    case Sentence::Panda: return buildPanda(s, imu, out, cap);
    case Sentence::Paogi: return buildPaogi(s, imu, out, cap);
    case Sentence::None: break;
    }
    return 0;
}

}  // namespace aow::nav
