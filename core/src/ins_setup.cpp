// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors

#include "aow/ins_setup.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace aow::ins {

namespace {

constexpr double kTolerance = 0.01;  // m
constexpr double kRange = 100.0;     // m, the receiver's limit for each component

bool near(const Vec& a, const Vec& b, double tol) {
    tol += 1e-9;  // 0.80 - 0.79 is a hair over 0.01 in binary
    return std::fabs(a.x - b.x) <= tol && std::fabs(a.y - b.y) <= tol && std::fabs(a.z - b.z) <= tol;
}

// The first three numbers after `key` in the line, if the line has the key.
bool numbersAfter(const std::string& line, const char* key, Vec& v) {
    const size_t at = line.find(key);
    if (at == std::string::npos) return false;
    std::istringstream in(line.substr(at + std::string(key).size()));
    return static_cast<bool>(in >> v.x >> v.y >> v.z);
}

std::string format(const Vec& v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f %.2f %.2f", v.x, v.y, v.z);
    return buf;
}

}  // namespace

Vec leverArm(const pgn::VehicleGeometry& g) {
    Vec v;
    v.x = (g.antennaOffsetCm - g.imuOffsetCm) / 100.0;
    v.y = (g.antennaPivotCm - g.imuPivotCm) / 100.0;
    v.z = (static_cast<int>(g.antennaHeightCm) - static_cast<int>(g.imuHeightCm)) / 100.0;
    return v;
}

ReceiverConfig parseConfig(const std::string& configCommands) {
    ReceiverConfig c;
    std::istringstream in(configCommands);
    for (std::string line; std::getline(in, line);) {
        Vec v;
        if (numbersAfter(line, "IMUTOANT OFFSET", v)) {
            c.hasImuToAnt = true;
            c.imuToAnt = v;
        } else if (numbersAfter(line, "INSSOL OFFSET", v)) {
            c.hasInsSol = true;
            c.insSol = v;
        } else if (numbersAfter(line, "INS ANGLE", v)) {
            const auto zero = [](double a) { return std::fabs(std::fmod(a, 36000.0)) < 0.5; };
            c.square = zero(v.x) && zero(v.y) && zero(v.z);
        }
    }
    return c;
}

std::string imuToAntCommand(const Vec& v) { return "CONFIG IMUTOANT OFFSET " + format(v) + " 0.03 0.03 0.03"; }
std::string insSolCommand(const Vec& v) { return "CONFIG INSSOL OFFSET " + format(v); }

Decision decide(const ReceiverConfig& have, const Vec& want, std::vector<std::string>& commands) {
    commands.clear();
    if (!have.hasImuToAnt) return Decision::NoIns;
    if (!have.square) return Decision::Rotated;
    if (std::fabs(want.x) > kRange || std::fabs(want.y) > kRange || std::fabs(want.z) > kRange) return Decision::OutOfRange;
    if (near(have.imuToAnt, want, kTolerance) && have.hasInsSol && near(have.insSol, want, kTolerance))
        return Decision::AlreadySet;
    commands = {imuToAntCommand(want), insSolCommand(want)};
    return Decision::Write;
}

void LeverArmJob::onGeometry(const pgn::VehicleGeometry& g) {
    const Vec v = leverArm(g);
    // The app sends the same geometry again and again: only a change starts a new check.
    if (haveGeometry_ && imuEntered_ == g.imuEntered && (!g.imuEntered || near(v, want_, 0.005))) return;
    haveGeometry_ = true;
    imuEntered_ = g.imuEntered;
    want_ = g.imuEntered ? v : Vec{};
    settled_ = false;
    failed_ = false;
}

void LeverArmJob::recheck() {
    settled_ = false;
    failed_ = false;
}

bool LeverArmJob::due(const Conditions& c, uint64_t nowMs) {
    if (!haveGeometry_ || settled_) return false;
    if (!imuEntered_) {
        message_ = "No IMU position in AgOpenWeb (Vehicle Configuration, Roll): the receiver's lever arm is left as it is";
        return false;
    }
    if (!c.receiverIns) {
        message_ = "The receiver sends no INS solution: no lever arm to set";
        return false;
    }
    if (c.externalImu) {
        message_ = "An external IMU is in use, so the IMU position in AgOpenWeb is taken to be its: the receiver's lever arm is left as it is";
        return false;
    }
    if (c.steering || c.speedKph > kStoppedKph) {
        message_ = "Lever arm " + format(want_) + " m to set: waiting for steering off and the vehicle stopped";
        return false;
    }
    if (failed_ && nowMs - failedAtMs_ < kRetryMs) return false;
    message_ = "Checking the receiver's lever arm against " + format(want_) + " m";
    return true;
}

void LeverArmJob::finished(bool ok, Decision d, uint64_t nowMs) {
    if (!ok) {
        failed_ = true;
        failedAtMs_ = nowMs;
        message_ = "The receiver did not answer: trying again in a minute";
        return;
    }
    settled_ = true;
    failed_ = false;
    switch (d) {
    case Decision::Write:
        message_ = "Lever arm set to " + format(want_) + " m (IMU to antenna: right, forward, up) and saved on the receiver";
        break;
    case Decision::AlreadySet:
        message_ = "The receiver's lever arm is " + format(want_) + " m (IMU to antenna: right, forward, up)";
        break;
    case Decision::NoIns:
        message_ = "The receiver lists no IMU lever arm (not a UM981): nothing set";
        break;
    case Decision::Rotated:
        message_ = "The receiver is mounted turned (CONFIG INS ANGLE is not 0 0 0): lever arm not set; set it on the GPS page";
        break;
    case Decision::OutOfRange:
        message_ = "Lever arm " + format(want_) + " m is beyond the receiver's 100 m: check the positions in AgOpenWeb";
        break;
    }
}

}  // namespace aow::ins
