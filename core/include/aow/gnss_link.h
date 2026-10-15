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
#include "aow/pgn.h"
#include "aow/port_monitor.h"

namespace aow {

class GnssLink {
public:
    GnssLink(hal::SerialPort& receiver, hal::UdpLink& app, hal::Clock& clock);

    // Drains the receiver's serial port. Call when it is readable.
    void pollReceiver();
    // Bytes from the receiver, read by the caller (the daemon reads every port itself).
    void feedReceiver(const uint8_t* data, size_t len);
    // Where corrections go: the receiver in use, which can change (nullptr: none, dropped).
    void setReceiver(hal::SerialPort* receiver) { receiver_ = receiver; }

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
    // Sentences are written whole or not at all: one that does not fit behind what the port
    // still has to send is dropped and counted.
    void setNmeaOut(hal::SerialPort* port, uint8_t sentences, uint8_t rateHz);
    // Corrected position for NMEA out (PGN 100, as AgOpenGPS's GPS_Out). On, and while the app
    // sends it, the receiver's latest sentences go out when each PGN 100 arrives, with its
    // position in GGA and RMC and its heading as the course in VTG and RMC and in HDT; the rest
    // is the receiver's. No PGN 100 for kCorrectedTimeoutUs: the receiver's sentences as received.
    void setNmeaCorrected(bool on) { corrected_ = on; }
    void onCorrectedFix(const pgn::CorrectedFix& f);
    // NMEA out is carrying the app's position now (else the receiver's).
    bool correctedActive() const { return corrected_ && correctedActive(clock_.nowUs()); }
    static constexpr uint64_t kCorrectedTimeoutUs = 1'000'000;
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
        uint32_t correctedFixes = 0;    // PGN 100 received
        uint32_t correctedSent = 0;     // sentences written to NMEA out with its position in
        uint32_t nmeaOutDropped = 0;    // sentences NMEA out had no room for
    };
    const Counters& counters() const { return counters_; }

    // What went out, for the Serial Communications page's Going Out Now: the last sentence NMEA
    // out wrote of each type (Settings::Ports::Sentence bit order: GGA, VTG, RMC, ZDA, HDT),
    // without its CRLF, and when (0: never); and RTCM out's bytes, as a port monitor sees them.
    static constexpr size_t kOutChars = 120;
    const char* nmeaOutLast(int type) const { return lastOut_[type]; }
    uint64_t nmeaOutLastUs(int type) const { return lastOutUs_[type]; }
    const PortMonitor& rtcmOutMonitor() const { return rtcmOutMon_; }
    const nmea::SentenceReader& reader() const { return reader_; }

private:
    // The current sentence as the receiver sent it: body, checksum, CRLF. Returns its length.
    size_t rawSentence(char* out, size_t cap) const;
    void sendRaw();
    void nmeaOut(uint64_t nowUs);
    bool correctedActive(uint64_t nowUs) const { return haveFix_ && nowUs - fixAtUs_ < kCorrectedTimeoutUs; }
    bool nmeaDue(int type, uint64_t nowUs);
    bool writeNmea(int type, const char* data, size_t len);
    void toReceiver(const uint8_t* data, size_t len);

    hal::SerialPort* receiver_;
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
    // What NMEA out still has to write of a sentence the port took only part of.
    char nmeaPending_[nmea::SentenceReader::kMax + 16];
    size_t nmeaPendingLen_ = 0;
    // Corrected position: the receiver's latest sentence of each type (body, no checksum), and
    // whether it has gone out yet.
    struct Held {
        char body[nmea::SentenceReader::kMax + 1];
        size_t len = 0;
        bool fresh = false;
    };
    Held held_[5];
    bool corrected_ = false;
    bool haveFix_ = false;
    uint64_t fixAtUs_ = 0;
    hal::SerialPort* rtcmOut_ = nullptr;
    char lastOut_[5][kOutChars + 1] = {};
    uint64_t lastOutUs_[5] = {};
    PortMonitor rtcmOutMon_;
};

}  // namespace aow
