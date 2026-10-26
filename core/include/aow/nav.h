// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
// Ported from Firmware_Teensy_AiO-New-Dawn, copyright 2025 by the AOG Group.
//
// The navigation sentences the app accepts: $PANDA (single antenna) and $PAOGI (dual
// antenna or INS). Ported from the Teensy firmware's NAVProcessor. The IMU fields come from an
// external IMU (aow/imu.h) when one is sending, else the firmware's "no IMU" values.
// AOW HAT: pitch goes out in degrees in both sentences, as the app reads it; the firmware sends
// an IMU's pitch x10.
#pragma once

#include <cstddef>
#include <cstdint>

#include "aow/gnss.h"
#include "aow/imu.h"

namespace aow::nav {

enum class Sentence : uint8_t { None, Panda, Paogi };

// Dual heading or INS -> PAOGI (even without a fix, so INS alignment shows in the app);
// otherwise PANDA once there is a fix and a position in the last 5 s.
Sentence select(const gnss::State& s, uint64_t nowUs);

// Writes the sentence, "*hh\r\n" included, NUL-terminated. Returns its length, 0 if it
// cannot be built from this state or does not fit. imu: the external IMU's latest data, or
// nullptr when there is none or it has gone quiet.
size_t buildPanda(const gnss::State& s, const imu::Data* imu, char* out, size_t cap);
size_t buildPaogi(const gnss::State& s, const imu::Data* imu, char* out, size_t cap);

// select() then the matching build. Returns 0 when nothing should be sent.
size_t build(const gnss::State& s, const imu::Data* imu, uint64_t nowUs, char* out, size_t cap);

}  // namespace aow::nav
