// Receiver to app, and corrections to receiver. Reads the GNSS serial stream, sends a PANDA
// or PAOGI to the app after every position sentence (as the firmware does), and writes
// RTCM from the app straight to the receiver.
#pragma once

#include <cstddef>
#include <cstdint>

#include "aow/gnss.h"
#include "aow/hal.h"
#include "aow/imu.h"
#include "aow/nmea.h"

namespace aow {

class GnssLink {
public:
    GnssLink(hal::SerialPort& receiver, hal::UdpLink& app, hal::Clock& clock);

    // Drains the receiver's serial port. Call when it is readable.
    void pollReceiver();

    // One RTCM datagram from the app. Datagrams under 5 bytes are ignored, as the firmware.
    void forwardRtcm(const uint8_t* data, size_t len);
    // Radio RTCM bytes (RS-232 #1): forwarded as they come.
    void forwardRadio(const uint8_t* data, size_t len);

    // The Device page's GPS passthrough: every sentence goes to the app as received, instead
    // of PANDA/PAOGI.
    void setPassThrough(bool on) { passThrough_ = on; }

    // An external IMU: its data goes into PANDA/PAOGI while it is sending.
    void setImu(const imu::Imu* imu) { imu_ = imu; }

    const gnss::State& state() const { return parser_.state(); }

    struct Counters {
        uint32_t sentences = 0;     // recognised and applied
        uint32_t navSent = 0;       // PANDA / PAOGI sent to the app
        uint32_t navSendErrors = 0;
        uint32_t rtcmPackets = 0;
        uint32_t rtcmBytesDropped = 0;  // serial port would not take them
        uint32_t radioBytes = 0;
    };
    const Counters& counters() const { return counters_; }
    const nmea::SentenceReader& reader() const { return reader_; }

private:
    void sendRaw();

    hal::SerialPort& receiver_;
    hal::UdpLink& app_;
    hal::Clock& clock_;
    nmea::SentenceReader reader_;
    gnss::Parser parser_;
    Counters counters_;
    bool passThrough_ = false;
    const imu::Imu* imu_ = nullptr;
};

}  // namespace aow
