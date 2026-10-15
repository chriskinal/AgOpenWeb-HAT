#include "aow/gnss_link.h"

#include "aow/nav.h"

#include <cmath>
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
    if (port != nmeaOut_) nmeaPendingLen_ = 0;
    nmeaOut_ = port;
    nmeaSentences_ = sentences;
    // A little under the interval, so a 10 Hz receiver at 5 Hz out sends every other one.
    nmeaIntervalUs_ = rateHz ? 1'000'000u / rateHz * 3 / 4 : 0;
}

namespace {

const char* const kTypes[] = {"GGA", "VTG", "RMC", "ZDA", "HDT"};  // Sentence bit order

// "$GNGGA,...": any talker; the type is the three letters after it. -1: none of ours.
int sentenceType(const char* s, size_t len) {
    if (len < 6 || s[0] != '$') return -1;
    for (int i = 0; i < 5; ++i)
        if (std::strncmp(s + 3, kTypes[i], 3) == 0) return i;
    return -1;
}

// ddmm.mmmmmmm (latitude, 2 degree digits) or dddmm.mmmmmmm (longitude, 3): GPS_Out's 7
// decimals of a minute, about 0.2 mm. Worked in whole 1e-7 minutes so the rounding carries
// into the degrees, and never through float, which would be metres off.
void formatDegMin(double deg, int degDigits, char* out, size_t cap) {
    const long long units = std::llround(std::fabs(deg) * 60.0 * 1e7);
    const long long perDeg = 60LL * 10'000'000;
    const long long d = units / perDeg, rest = units % perDeg;
    std::snprintf(out, cap, "%0*lld%02lld.%07lld", degDigits, d, rest / 10'000'000, rest % 10'000'000);
}

// Copies the sentence body with the fields in repl (indexed by field, the address being 0)
// put in place of the receiver's; nullptr keeps it. Appends the checksum and CRLF. Returns the
// length, 0 if it does not fit.
size_t rewrite(const char* in, size_t len, const char* const* repl, int nRepl, char* out, size_t cap) {
    size_t o = 0;
    int field = 0;
    size_t i = 0;
    while (i <= len) {
        size_t end = i;
        while (end < len && in[end] != ',') ++end;
        const char* r = field < nRepl ? repl[field] : nullptr;
        const char* src = r ? r : in + i;
        const size_t n = r ? std::strlen(r) : end - i;
        if (o + n + 1 + 8 > cap) return 0;
        std::memcpy(out + o, src, n);
        o += n;
        if (end < len) out[o++] = ',';
        i = end + 1;
        ++field;
    }
    o += static_cast<size_t>(std::snprintf(out + o, cap - o, "*%02X\r\n", nmea::xorChecksum(out + 1, o - 1)));
    return o;
}

}  // namespace

bool GnssLink::nmeaDue(int type, uint64_t nowUs) {
    if (nmeaIntervalUs_ && nmeaLastUs_[type] && nowUs - nmeaLastUs_[type] < nmeaIntervalUs_) return false;
    nmeaLastUs_[type] = nowUs;
    return true;
}

bool GnssLink::writeNmea(int type, const char* data, size_t len) {
    // The rest of a sentence the port took only part of goes first; until it has gone, new
    // ones are dropped whole rather than cut into it.
    if (nmeaPendingLen_) {
        const int w = nmeaOut_->write(reinterpret_cast<const uint8_t*>(nmeaPending_), nmeaPendingLen_);
        const size_t done = w > 0 ? static_cast<size_t>(w) : 0;
        std::memmove(nmeaPending_, nmeaPending_ + done, nmeaPendingLen_ - done);
        nmeaPendingLen_ -= done;
        if (nmeaPendingLen_) {
            ++counters_.nmeaOutDropped;
            counters_.outBytesDropped += static_cast<uint32_t>(len);
            return false;
        }
    }
    const int w = nmeaOut_->write(reinterpret_cast<const uint8_t*>(data), len);
    const size_t done = w > 0 ? static_cast<size_t>(w) : 0;
    if (done == 0) {
        ++counters_.nmeaOutDropped;
        counters_.outBytesDropped += static_cast<uint32_t>(len);
        return false;
    }
    if (done < len && len - done <= sizeof nmeaPending_) {
        std::memcpy(nmeaPending_, data + done, len - done);
        nmeaPendingLen_ = len - done;
    }
    ++counters_.nmeaOutSent;
    size_t text = len;
    while (text && (data[text - 1] == '\r' || data[text - 1] == '\n')) --text;
    if (text > kOutChars) text = kOutChars;
    std::memcpy(lastOut_[type], data, text);
    lastOut_[type][text] = '\0';
    lastOutUs_[type] = clock_.nowUs();
    return true;
}

void GnssLink::nmeaOut(uint64_t nowUs) {
    const char* s = reader_.sentence();
    const int type = sentenceType(s, reader_.length());
    if (type < 0 || !(nmeaSentences_ & (1u << type))) return;
    if (corrected_) {
        Held& h = held_[type];
        std::memcpy(h.body, s, reader_.length());
        h.len = reader_.length();
        h.fresh = true;
        if (correctedActive(nowUs)) return;  // it goes out with the next PGN 100
        h.fresh = false;
    }
    if (!nmeaDue(type, nowUs)) return;
    char out[nmea::SentenceReader::kMax + 16];
    const size_t len = rawSentence(out, sizeof out);
    if (len) writeNmea(type, out, len);
}

void GnssLink::onCorrectedFix(const pgn::CorrectedFix& f) {
    ++counters_.correctedFixes;
    const uint64_t now = clock_.nowUs();
    haveFix_ = true;
    fixAtUs_ = now;
    if (!nmeaOut_ || !corrected_) return;

    char lat[48], lon[48], course[16];
    formatDegMin(f.lat, 2, lat, sizeof lat);
    formatDegMin(f.lon, 3, lon, sizeof lon);
    std::snprintf(course, sizeof course, "%05.1f", f.heading);
    const char* ns = f.lat < 0 ? "S" : "N";
    const char* ew = f.lon < 0 ? "W" : "E";
    const char* hdg = f.hasHeading ? course : nullptr;
    const char* const gga[] = {nullptr, nullptr, lat, ns, lon, ew};
    const char* const vtg[] = {nullptr, hdg, nullptr, hdg};
    const char* const rmc[] = {nullptr, nullptr, nullptr, lat, ns, lon, ew, nullptr, hdg};
    const char* const hdt[] = {nullptr, hdg};
    struct { const char* const* repl; int n; } const kRepl[] = {{gga, 6}, {vtg, 4}, {rmc, 9}, {nullptr, 0}, {hdt, 2}};

    // Each of the receiver's sentences once: ZDA at 1 Hz goes out once a second, not with
    // every PGN 100.
    for (int type = 0; type < 5; ++type) {
        Held& h = held_[type];
        if (!(nmeaSentences_ & (1u << type)) || !h.fresh) continue;
        h.fresh = false;
        if (!nmeaDue(type, now)) continue;
        char out[nmea::SentenceReader::kMax + 64];
        const size_t len = rewrite(h.body, h.len, kRepl[type].repl, kRepl[type].n, out, sizeof out);
        if (len && writeNmea(type, out, len)) ++counters_.correctedSent;
    }
}

void GnssLink::toReceiver(const uint8_t* data, size_t len) {
    const int written = receiver_ ? receiver_->write(data, len) : 0;
    if (written < static_cast<int>(len))
        counters_.rtcmBytesDropped += static_cast<uint32_t>(len - (written > 0 ? written : 0));
    if (!rtcmOut_) return;
    const int out = rtcmOut_->write(data, len);
    if (out > 0) rtcmOutMon_.feed(data, static_cast<size_t>(out), clock_.nowUs());
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
