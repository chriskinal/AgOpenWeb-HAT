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

    // NMEA out (AOW HAT): the receiver's sentences of the chosen types (Settings::Ports::Sentence
    // bits), as received, to a serial port, e.g. for a rate controller. rateHz 0: every one;
    // else at most that many of each type a second. nullptr: off.
    void setNmeaOut(hal::SerialPort* port, uint8_t sentences, uint8_t rateHz);
    // RTCM out (AOW HAT): a copy of every correction byte the receiver is sent, from the app and
    // from the radio, to a serial port. nullptr: off.
    void setRtcmOut(hal::SerialPort* port) { rtcmOut_ = port; }

    const gnss::State& state() const { return parser_.state(); }

    struct Counters {
        uint32_t sentences = 0;     // recognised and applied
        uint32_t navSent = 0;       // PANDA / PAOGI sent to the app
        uint32_t navSendErrors = 0;
        uint32_t rtcmPackets = 0;
        uint32_t rtcmBytesDropped = 0;  // serial port would not take them
        uint32_t radioBytes = 0;
        uint32_t nmeaOutSent = 0;       // sentences written to NMEA out
        uint32_t rtcmOutBytes = 0;      // bytes written to RTCM out
        uint32_t outBytesDropped = 0;   // NMEA or RTCM out port would not take them
    };
    const Counters& counters() const { return counters_; }
    const nmea::SentenceReader& reader() const { return reader_; }

private:
    // The current sentence as the receiver sent it: body, checksum, CRLF. Returns its length.
    size_t rawSentence(char* out, size_t cap) const;
    void sendRaw();
    void nmeaOut(uint64_t nowUs);
    void toReceiver(const uint8_t* data, size_t len);

    hal::SerialPort& receiver_;
    hal::UdpLink& app_;
    hal::Clock& clock_;
    nmea::SentenceReader reader_;
    gnss::Parser parser_;
    Counters counters_;
    bool passThrough_ = false;
    const imu::Imu* imu_ = nullptr;
    hal::SerialPort* nmeaOut_ = nullptr;
    uint8_t nmeaSentences_ = 0;
    uint64_t nmeaIntervalUs_ = 0;
    uint64_t nmeaLastUs_[5] = {};  // per sentence type, in Sentence bit order
    hal::SerialPort* rtcmOut_ = nullptr;
};

}  // namespace aow
