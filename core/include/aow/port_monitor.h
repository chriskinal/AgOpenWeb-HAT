// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
//
// What is coming in on a serial port, for the Serial Communications page: the first characters
// of the last few lines, and the RTCM message numbers seen lately. Fed every byte read from the
// port, whatever it is used for. Fixed size, no allocation: it runs on the control thread.
#pragma once

#include <cstddef>
#include <cstdint>

namespace aow {

class PortMonitor {
public:
    static constexpr int kLines = 4;        // the last this many lines
    static constexpr int kLineChars = 20;   // the first this many characters of each
    static constexpr int kRtcmTypes = 10;   // RTCM message numbers remembered
    static constexpr uint64_t kRtcmAgeUs = 5'000'000;  // forgotten after this long unseen

    void feed(const uint8_t* buf, size_t n, uint64_t nowUs);

    uint32_t bytes() const { return bytes_; }
    uint64_t lastUs() const { return lastUs_; }

    // The last lines, oldest first, as NUL-terminated text: printable ASCII, anything else as
    // '.'. A run of 200 bytes with no line end counts as a line (binary data).
    int lineCount() const { return lineCount_; }
    const char* line(int i) const;

    // RTCM 3 frames with a good CRC, all time.
    uint32_t rtcmFrames() const { return rtcmFrames_; }
    // RTCM 3 message numbers with a good CRC seen in the last kRtcmAgeUs, ascending.
    int rtcmTypes(uint64_t nowUs, uint16_t* out, int cap) const;

private:
    void endLine();
    void feedRtcm(uint8_t b, uint64_t nowUs);

    uint32_t bytes_ = 0;
    uint64_t lastUs_ = 0;

    char lines_[kLines][kLineChars + 1] = {};
    int lineCount_ = 0, lineHead_ = 0;  // ring: lineHead_ is the next to write
    char cur_[kLineChars + 1] = {};
    int curLen_ = 0, curRun_ = 0;

    uint8_t frame_[3 + 1023 + 3] = {};
    size_t frameLen_ = 0, frameWant_ = 0;
    struct Seen { uint16_t type; uint64_t us; };
    Seen rtcm_[kRtcmTypes] = {};
    uint32_t rtcmFrames_ = 0;
};

}  // namespace aow
