// Works out what is plugged into a serial port from what it sends: NMEA (or Unicore ASCII)
// sentences with good checksums make it a GNSS receiver (a base sends RTCM too, and a port first
// taken for a radio becomes one), IMU packets (BNO085 RVC, TM171) an IMU, RTCM 3 frames with
// good CRCs an RTK radio. On auto rate it tries the common rates in
// turn until one of those parses. Once identified it stays so while that kind of data keeps
// coming; after kLostUs without, it listens afresh. Pure logic: the caller reads the port and
// sets the rate (baud()) when tick() says it changed.
#pragma once

#include <cstddef>
#include <cstdint>

#include "aow/imu.h"
#include "aow/nmea.h"
#include "aow/port_monitor.h"

namespace aow {

class SerialDetector {
public:
    enum class Kind : uint8_t { Unknown, Gnss, Radio, Imu };

    static constexpr uint64_t kTryUs = 2'500'000;   // listening at one rate
    static constexpr uint64_t kLostUs = 5'000'000;  // identified, but nothing of its kind since
    static constexpr int kNmeaSentences = 2;
    static constexpr unsigned kRates[] = {115200, 460800, 38400, 57600, 9600, 230400, 19200};

    // pinnedBaud 0: auto, trying kRates from the first.
    explicit SerialDetector(unsigned pinnedBaud = 0, uint64_t nowUs = 0);

    void feed(const uint8_t* buf, size_t n, uint64_t nowUs);
    // Returns true when the rate to listen at has changed.
    bool tick(uint64_t nowUs);
    // Start again from nothing (the device went away, or a new pinned rate).
    void reset(unsigned pinnedBaud, uint64_t nowUs);

    Kind kind() const { return kind_; }
    unsigned baud() const { return pinned_ ? pinned_ : kRates[rateIndex_]; }
    bool autoBaud() const { return pinned_ == 0; }
    uint64_t lastValidUs() const { return lastValidUs_; }
    const PortMonitor& monitor() const { return monitor_; }
    const imu::Imu& imu() const { return imu_; }

private:
    void restartTry(uint64_t nowUs);

    unsigned pinned_ = 0;
    int rateIndex_ = 0;
    uint64_t tryStartUs_ = 0, lastValidUs_ = 0;
    Kind kind_ = Kind::Unknown;
    PortMonitor monitor_;
    nmea::SentenceReader nmea_;
    imu::Imu imu_;
    int sentences_ = 0;
    uint32_t imuPackets_ = 0, rtcmFrames_ = 0;  // at the start of this try
};

const char* kindName(SerialDetector::Kind k);  // "unknown", "gnss", "radio", "imu"

}  // namespace aow
