#include "aow/port_monitor.h"

namespace aow {

namespace {

constexpr int kBinaryRun = 200;  // bytes with no line end that count as a line

// CRC-24Q, as RTCM 3 uses it (polynomial 0x1864CFB, init 0).
uint32_t crc24q(const uint8_t* d, size_t n) {
    uint32_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        crc ^= static_cast<uint32_t>(d[i]) << 16;
        for (int b = 0; b < 8; ++b) {
            crc <<= 1;
            if (crc & 0x1000000) crc ^= 0x1864CFB;
        }
    }
    return crc & 0xFFFFFF;
}

}  // namespace

void PortMonitor::feed(const uint8_t* buf, size_t n, uint64_t nowUs) {
    if (!n) return;
    bytes_ += static_cast<uint32_t>(n);
    lastUs_ = nowUs;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = buf[i];
        feedRtcm(c, nowUs);
        if (c == '\r' || c == '\n') {
            if (curRun_) endLine();
            continue;
        }
        if (curLen_ < kLineChars) cur_[curLen_++] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
        if (++curRun_ >= kBinaryRun) endLine();
    }
}

void PortMonitor::endLine() {
    cur_[curLen_] = '\0';
    for (int i = 0; i <= curLen_; ++i) lines_[lineHead_][i] = cur_[i];
    lineHead_ = (lineHead_ + 1) % kLines;
    if (lineCount_ < kLines) ++lineCount_;
    curLen_ = 0;
    curRun_ = 0;
}

const char* PortMonitor::line(int i) const {
    if (i < 0 || i >= lineCount_) return "";
    return lines_[(lineHead_ - lineCount_ + i + kLines) % kLines];
}

void PortMonitor::feedRtcm(uint8_t b, uint64_t nowUs) {
    // 0xD3, 6 reserved bits and a 10-bit length, the message, a 24-bit CRC.
    if (frameLen_ == 0) {
        if (b == 0xD3) frame_[frameLen_++] = b;
        return;
    }
    frame_[frameLen_++] = b;
    if (frameLen_ == 3) {
        frameWant_ = 3 + (((frame_[1] & 0x03u) << 8) | frame_[2]) + 3;
        if ((frame_[1] & 0xFC) || frameWant_ < 3 + 2 + 3) frameLen_ = 0;  // not a frame
        return;
    }
    if (frameLen_ < 3 || frameLen_ < frameWant_) return;
    const size_t len = frameLen_;
    frameLen_ = 0;
    const uint32_t crc = (static_cast<uint32_t>(frame_[len - 3]) << 16) | (frame_[len - 2] << 8) | frame_[len - 1];
    if (crc24q(frame_, len - 3) != crc) return;
    ++rtcmFrames_;
    const uint16_t type = static_cast<uint16_t>((frame_[3] << 4) | (frame_[4] >> 4));
    // Update its entry, else take an empty or the oldest one.
    int slot = 0;
    for (int i = 0; i < kRtcmTypes; ++i) {
        if (rtcm_[i].us && rtcm_[i].type == type) { slot = i; break; }
        if (rtcm_[i].us < rtcm_[slot].us) slot = i;
    }
    rtcm_[slot] = {type, nowUs ? nowUs : 1};
}

int PortMonitor::rtcmTypes(uint64_t nowUs, uint16_t* out, int cap) const {
    int n = 0;
    for (const Seen& s : rtcm_) {
        if (!s.us || nowUs - s.us > kRtcmAgeUs || n >= cap) continue;
        int j = n++;  // insertion sort: ascending
        while (j > 0 && out[j - 1] > s.type) {
            out[j] = out[j - 1];
            --j;
        }
        out[j] = s.type;
    }
    return n;
}

}  // namespace aow
