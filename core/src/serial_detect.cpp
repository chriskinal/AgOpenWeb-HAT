// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors

#include "aow/serial_detect.h"

namespace aow {

constexpr unsigned SerialDetector::kRates[];

SerialDetector::SerialDetector(unsigned pinnedBaud, uint64_t nowUs) { reset(pinnedBaud, nowUs); }

void SerialDetector::reset(unsigned pinnedBaud, uint64_t nowUs) {
    pinned_ = pinnedBaud;
    rateIndex_ = 0;
    kind_ = Kind::Unknown;
    lastValidUs_ = 0;
    restartTry(nowUs);
}

void SerialDetector::restartTry(uint64_t nowUs) {
    tryStartUs_ = nowUs;
    nmea_ = nmea::SentenceReader();
    imu_ = imu::Imu();
    sentences_ = 0;
    imuPackets_ = 0;
    rtcmFrames_ = monitor_.rtcmFrames();
}

void SerialDetector::feed(const uint8_t* buf, size_t n, uint64_t nowUs) {
    monitor_.feed(buf, n, nowUs);
    imu_.feed(buf, n, nowUs);
    int good = 0;
    for (size_t i = 0; i < n; ++i)
        if (nmea_.feed(static_cast<char>(buf[i]))) ++good;
    sentences_ += good;
    const bool rtcm = monitor_.rtcmFrames() != rtcmFrames_;
    const bool imuData = imu_.type() != imu::Type::None && imu_.packets() != imuPackets_;
    rtcmFrames_ = monitor_.rtcmFrames();
    imuPackets_ = imu_.packets();

    if (kind_ == Kind::Unknown) {
        // A base receiver sends NMEA and RTCM: NMEA decides.
        if (sentences_ >= kNmeaSentences) kind_ = Kind::Gnss;
        else if (imu_.type() != imu::Type::None) kind_ = Kind::Imu;
        else if (rtcm) kind_ = Kind::Radio;
        if (kind_ != Kind::Unknown) lastValidUs_ = nowUs;
        return;
    }
    // A base receiver that sent RTCM before its first sentences: a receiver after all.
    if (kind_ == Kind::Radio && sentences_ >= kNmeaSentences) kind_ = Kind::Gnss;
    if ((kind_ == Kind::Gnss && good) || (kind_ == Kind::Imu && imuData) || (kind_ == Kind::Radio && rtcm))
        lastValidUs_ = nowUs;
}

bool SerialDetector::tick(uint64_t nowUs) {
    if (kind_ != Kind::Unknown) {
        if (nowUs - lastValidUs_ <= kLostUs) return false;
        kind_ = Kind::Unknown;  // gone quiet: listen afresh, at the same rate first
        restartTry(nowUs);
        return false;
    }
    if (pinned_ || nowUs - tryStartUs_ < kTryUs) return false;
    rateIndex_ = (rateIndex_ + 1) % static_cast<int>(sizeof kRates / sizeof kRates[0]);
    restartTry(nowUs);
    return true;
}

const char* kindName(SerialDetector::Kind k) {
    switch (k) {
    case SerialDetector::Kind::Gnss: return "gnss";
    case SerialDetector::Kind::Radio: return "radio";
    case SerialDetector::Kind::Imu: return "imu";
    case SerialDetector::Kind::Unknown: break;
    }
    return "unknown";
}

}  // namespace aow
