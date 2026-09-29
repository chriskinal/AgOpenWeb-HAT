#include "aow/nmea.h"

#include <cstdlib>
#include <cstring>

namespace aow::nmea {

namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

}  // namespace

uint8_t xorChecksum(const char* s, size_t len) {
    uint8_t x = 0;
    for (size_t i = 0; i < len; ++i) x ^= static_cast<uint8_t>(s[i]);
    return x;
}

uint32_t crc32(const char* s, size_t len) {
    uint32_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint8_t>(s[i]);
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

bool SentenceReader::feed(char c) {
    // A start character always begins a new sentence, whatever state we were in.
    if (c == '$' || c == '#') {
        state_ = Body;
        unicore_ = (c == '#');
        buf_[0] = c;
        len_ = 1;
        return false;
    }

    switch (state_) {
    case WaitStart:
        return false;

    case Body:
        if (c == '*') {
            state_ = Checksum;
            sum_ = 0;
            sumDigits_ = 0;
        } else if (c == '\r' || c == '\n') {
            state_ = WaitStart;  // no checksum: dropped
        } else if (len_ < kMax) {
            buf_[len_++] = c;
        } else {
            ++overflows_;
            state_ = WaitStart;
        }
        return false;

    case Checksum: {
        const int v = hexValue(c);
        if (v < 0) {
            state_ = WaitStart;
            return false;
        }
        sum_ = sum_ << 4 | static_cast<uint32_t>(v);
        if (++sumDigits_ < (unicore_ ? 8 : 2)) return false;

        state_ = WaitStart;
        buf_[len_] = '\0';
        const bool ok = unicore_ ? crc32(buf_ + 1, len_ - 1) == sum_
                                 : xorChecksum(buf_ + 1, len_ - 1) == sum_;
        if (!ok) ++badChecksums_;
        return ok;
    }
    }
    return false;
}

Fields::Fields(const char* body, size_t len) {
    if (len > SentenceReader::kMax) len = SentenceReader::kMax;
    std::memcpy(buf_, body, len);
    buf_[len] = '\0';
    f_[n_++] = buf_;
    for (size_t i = 0; i < len && n_ < kMax; ++i) {
        if (buf_[i] == ',' || buf_[i] == ';') {
            buf_[i] = '\0';
            f_[n_++] = buf_ + i + 1;
        }
    }
}

double Fields::num(int i) const { return std::strtod((*this)[i], nullptr); }

int Fields::integer(int i) const { return std::atoi((*this)[i]); }

bool Fields::startsWith(int i, const char* prefix) const {
    return std::strncmp((*this)[i], prefix, std::strlen(prefix)) == 0;
}

}  // namespace aow::nmea
