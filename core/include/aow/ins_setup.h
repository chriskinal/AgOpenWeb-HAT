// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
//
// The UM981's INS lever arm, from AgOpenWeb's vehicle geometry (PGN 247, issue #2).
//
// The receiver needs the vector from its IMU to the antenna's phase centre (CONFIG IMUTOANT
// OFFSET, UM981 commands reference §1.5), in its own frame: X right, Y forward, Z up while it is
// mounted square (CONFIG INS ANGLE 0 0 0, §1.2). Its INSPVAXA, which the daemon reads, reports
// the position of the module centre plus CONFIG INSSOL OFFSET (§1.6): set to the same vector, the
// position is the antenna's, which is what the app's antenna settings assume. Both come from the
// antenna and IMU positions the operator enters in AgOpenWeb.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aow/pgn.h"

namespace aow::ins {

struct Vec {
    double x = 0, y = 0, z = 0;  // metres: right, forward, up
};

// The antenna's position minus the IMU's. Meaningful only when g.imuEntered.
Vec leverArm(const pgn::VehicleGeometry& g);

// What the receiver's CONFIG list says about its INS.
struct ReceiverConfig {
    bool hasImuToAnt = false;  // "CONFIG IMUTOANT OFFSET x y z a b c": a receiver with an INS
    Vec imuToAnt;
    bool hasInsSol = false;    // "CONFIG INSSOL OFFSET x y z"
    Vec insSol;
    bool square = true;        // "CONFIG INS ANGLE x y z" all 0 (0.01 degree units), or not listed
};
// configCommands: one CONFIG command a line, as Um98x reads them.
ReceiverConfig parseConfig(const std::string& configCommands);

enum class Decision {
    Write,       // either offset is more than 1 cm from the wanted one: commands filled in
    AlreadySet,  // both within 1 cm
    NoIns,       // the receiver lists no IMUTOANT: not a UM981
    Rotated,     // mounted turned: the vector would have to be turned too
    OutOfRange,  // beyond the receiver's ±100 m
};
Decision decide(const ReceiverConfig& have, const Vec& want, std::vector<std::string>& commands);

// The two commands for a lever arm. The measurement error given with IMUTOANT is 3 cm a side:
// positions taken with a tape.
std::string imuToAntCommand(const Vec& v);
std::string insSolCommand(const Vec& v);

// When to talk to the receiver. Reading its CONFIG stops the GNSS stream for a few seconds, so
// it is done once for each new geometry (the app resends the same one often), only with an INS
// receiver, no external IMU (the IMU position in the app is then the external IMU's), steering
// off and the vehicle stopped; after a receiver that did not answer, again a minute later.
class LeverArmJob {
public:
    struct Conditions {
        bool receiverIns = false;  // the receiver sends INS solutions
        bool externalImu = false;
        bool steering = false;
        float speedKph = 0;
    };

    void onGeometry(const pgn::VehicleGeometry& g);
    // The receiver's settings were written some other way (the GPS page): check them again.
    void recheck();
    // True when the receiver should be read now; call finished() after.
    bool due(const Conditions& c, uint64_t nowMs);
    const Vec& want() const { return want_; }
    // ok false: the receiver did not answer (d is then ignored).
    void finished(bool ok, Decision d, uint64_t nowMs);
    // What is happening, for the status page and the log.
    const std::string& message() const { return message_; }

    static constexpr uint64_t kRetryMs = 60'000;
    static constexpr float kStoppedKph = 1.0f;

private:
    bool haveGeometry_ = false;
    bool imuEntered_ = false;
    Vec want_;
    bool settled_ = false;  // done for this geometry
    bool failed_ = false;
    uint64_t failedAtMs_ = 0;
    std::string message_ = "Waiting for the vehicle geometry from AgOpenWeb";
};

}  // namespace aow::ins
