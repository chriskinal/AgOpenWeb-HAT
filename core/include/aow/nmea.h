// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
//
// Sentence framing for the GNSS receiver's text output.
//
//   $...*hh          NMEA: XOR of the bytes between '$' and '*'
//   #...*hhhhhhhh    Unicore ASCII (INSPVAA, INSPVAXA): CRC-32 of the bytes between '#' and '*'
//
// Sentences without a checksum are dropped.
#pragma once

#include <cstddef>
#include <cstdint>

namespace aow::nmea {

uint8_t xorChecksum(const char* s, size_t len);
uint32_t crc32(const char* s, size_t len);  // the Unicore/NovAtel CRC (reflected 0xEDB88320, init 0)

class SentenceReader {
public:
    static constexpr size_t kMax = 512;

    // Feeds one byte. Returns true when a sentence has just completed with a good checksum;
    // sentence() then holds it from the '$' or '#' up to, not including, the '*'.
    bool feed(char c);

    const char* sentence() const { return buf_; }
    size_t length() const { return len_; }
    uint32_t badChecksums() const { return badChecksums_; }
    uint32_t overflows() const { return overflows_; }

private:
    enum State : uint8_t { WaitStart, Body, Checksum };

    char buf_[kMax + 1] = {};
    size_t len_ = 0;
    State state_ = WaitStart;
    bool unicore_ = false;
    uint32_t sum_ = 0;
    uint8_t sumDigits_ = 0;
    uint32_t badChecksums_ = 0;
    uint32_t overflows_ = 0;
};

// Splits a sentence (as SentenceReader gives it, without the lead '$' or '#') on ',' and ';'
// into NUL-terminated fields. Keeps its own copy.
class Fields {
public:
    static constexpr int kMax = 40;

    explicit Fields(const char* body, size_t len);
    int count() const { return n_; }
    // Missing fields read as "".
    const char* operator[](int i) const { return i < n_ ? f_[i] : ""; }
    bool empty(int i) const { return !*(*this)[i]; }
    double num(int i) const;  // 0 if empty or missing
    int integer(int i) const;
    bool startsWith(int i, const char* prefix) const;

private:
    char buf_[SentenceReader::kMax + 1];
    const char* f_[kMax];
    int n_ = 0;
};

}  // namespace aow::nmea
