#include "aow/gnss_link.h"

#include "aow/nav.h"

#include <cstdio>
#include <cstring>

namespace aow {

GnssLink::GnssLink(hal::SerialPort& receiver, hal::UdpLink& app, hal::Clock& clock)
    : receiver_(&receiver), app_(app), clock_(clock) {}

void GnssLink::pollReceiver() {
    uint8_t buf[1024];
    int n;
    while (receiver_ && (n = receiver_->read(buf, sizeof buf)) > 0) feedReceiver(buf, static_cast<size_t>(n));
}

void GnssLink::feedReceiver(const uint8_t* buf, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (!reader_.feed(static_cast<char>(buf[i]))) continue;
        const uint64_t now = clock_.nowUs();
        if (nmeaOut_) nmeaOut(now);
        if (passThrough_) {
            sendRaw();
            parser_.parse(reader_.sentence(), now);  // keep the state current for the status page
            continue;
        }
        const gnss::Message m = parser_.parse(reader_.sentence(), now);
        if (m == gnss::Message::None) continue;
        ++counters_.sentences;
        if (!gnss::carriesPosition(m)) continue;

        char out[256];
        const imu::Data* imu = imu_ && imu_->valid(now) ? &imu_->data() : nullptr;
        const size_t len = nav::build(parser_.state(), imu, now, out, sizeof out);
        if (!len) continue;
        if (app_.send(reinterpret_cast<const uint8_t*>(out), len)) ++counters_.navSent;
        else ++counters_.navSendErrors;
    }
}

size_t GnssLink::rawSentence(char* out, size_t cap) const {
    const char* s = reader_.sentence();
    const size_t len = reader_.length();
    if (len + 16 > cap) return 0;
    std::memcpy(out, s, len);
    const int n = s[0] == '#' ? std::snprintf(out + len, 16, "*%08x\r\n", nmea::crc32(s + 1, len - 1))
                              : std::snprintf(out + len, 16, "*%02X\r\n", nmea::xorChecksum(s + 1, len - 1));
    return len + static_cast<size_t>(n);
}

void GnssLink::sendRaw() {
    char out[nmea::SentenceReader::kMax + 16];  // on the stack: this runs on the control thread
    const size_t len = rawSentence(out, sizeof out);
    if (app_.send(reinterpret_cast<const uint8_t*>(out), len)) ++counters_.navSent;
    else ++counters_.navSendErrors;
}

void GnssLink::setNmeaOut(hal::SerialPort* port, uint8_t sentences, uint8_t rateHz) {
    nmeaOut_ = port;
    nmeaSentences_ = sentences;
    // A little under the interval, so a 10 Hz receiver at 5 Hz out sends every other one.
    nmeaIntervalUs_ = rateHz ? 1'000'000u / rateHz * 3 / 4 : 0;
}

void GnssLink::nmeaOut(uint64_t nowUs) {
    // "$GNGGA,...": any talker; the type is the three letters after it.
    const char* s = reader_.sentence();
    if (s[0] != '$' || reader_.length() < 6) return;
    static const char* const kTypes[] = {"GGA", "VTG", "RMC", "ZDA", "HDT"};
    for (int i = 0; i < 5; ++i) {
        if (!(nmeaSentences_ & (1u << i)) || std::strncmp(s + 3, kTypes[i], 3) != 0) continue;
        if (nmeaIntervalUs_ && nmeaLastUs_[i] && nowUs - nmeaLastUs_[i] < nmeaIntervalUs_) return;
        nmeaLastUs_[i] = nowUs;
        char out[nmea::SentenceReader::kMax + 16];
        const size_t len = rawSentence(out, sizeof out);
        const int written = nmeaOut_->write(reinterpret_cast<const uint8_t*>(out), len);
        if (written == static_cast<int>(len)) ++counters_.nmeaOutSent;
        else counters_.outBytesDropped += static_cast<uint32_t>(len - (written > 0 ? written : 0));
        return;
    }
}

void GnssLink::toReceiver(const uint8_t* data, size_t len) {
    const int written = receiver_ ? receiver_->write(data, len) : 0;
    if (written < static_cast<int>(len))
        counters_.rtcmBytesDropped += static_cast<uint32_t>(len - (written > 0 ? written : 0));
    if (!rtcmOut_) return;
    const int out = rtcmOut_->write(data, len);
    counters_.rtcmOutBytes += static_cast<uint32_t>(out > 0 ? out : 0);
    if (out < static_cast<int>(len)) counters_.outBytesDropped += static_cast<uint32_t>(len - (out > 0 ? out : 0));
}

void GnssLink::forwardRadio(const uint8_t* data, size_t len) {
    counters_.radioBytes += static_cast<uint32_t>(len);
    toReceiver(data, len);
}

void GnssLink::forwardRtcm(const uint8_t* data, size_t len) {
    if (len < 5) return;
    ++counters_.rtcmPackets;
    toReceiver(data, len);
}

}  // namespace aow
