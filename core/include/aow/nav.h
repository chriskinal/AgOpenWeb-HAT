// The navigation sentences the app accepts: $PANDA (single antenna) and $PAOGI (dual
// antenna or INS). Ported from the Teensy firmware's NAVProcessor. The HAT has no separate
// IMU, so PANDA always carries the firmware's "no IMU" values.
#pragma once

#include <cstddef>
#include <cstdint>

#include "aow/gnss.h"

namespace aow::nav {

enum class Sentence : uint8_t { None, Panda, Paogi };

// Dual heading or INS -> PAOGI (even without a fix, so INS alignment shows in the app);
// otherwise PANDA once there is a fix and a position in the last 5 s.
Sentence select(const gnss::State& s, uint64_t nowUs);

// Writes the sentence, "*hh\r\n" included, NUL-terminated. Returns its length, 0 if it
// cannot be built from this state or does not fit.
size_t buildPanda(const gnss::State& s, char* out, size_t cap);
size_t buildPaogi(const gnss::State& s, char* out, size_t cap);

// select() then the matching build. Returns 0 when nothing should be sent.
size_t build(const gnss::State& s, uint64_t nowUs, char* out, size_t cap);

}  // namespace aow::nav
