// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors

#include "aow/ins_setup.h"

#include <cmath>

#include "check.h"

using namespace aow;
using namespace aow::ins;

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

int main() {
    // The lever arm is the antenna minus the IMU: antenna 3.00 m high, 1.20 ahead of the pivot,
    // 0.15 left; IMU (the receiver, in the cab) 1.50 high, 0.40 ahead, 0.25 right.
    pgn::VehicleGeometry g;
    g.version = 1;
    g.imuEntered = true;
    g.antennaHeightCm = 300;
    g.antennaPivotCm = 120;
    g.antennaOffsetCm = -15;
    g.imuHeightCm = 150;
    g.imuPivotCm = 40;
    g.imuOffsetCm = 25;
    const Vec arm = leverArm(g);
    CHECK(near(arm.x, -0.40) && near(arm.y, 0.80) && near(arm.z, 1.50));  // left, ahead, up
    // An IMU above the antenna gives a negative Z (heights are unsigned in the PGN).
    g.imuHeightCm = 320;
    CHECK(near(leverArm(g).z, -0.20));
    g.imuHeightCm = 150;

    CHECK(imuToAntCommand(arm) == "CONFIG IMUTOANT OFFSET -0.40 0.80 1.50 0.03 0.03 0.03");
    CHECK(insSolCommand(arm) == "CONFIG INSSOL OFFSET -0.40 0.80 1.50");

    // The receiver's CONFIG: the firmware's UM981 defaults, as Um98x reads them.
    const std::string defaults = "CONFIG INS ANGLE 0 0 0\n"
                                 "CONFIG IMUTOANT OFFSET 0.00 0.00 0.00 0.01 0.01 0.01\n"
                                 "CONFIG INSSOL OFFSET 0.00 0.00 0.00\n"
                                 "CONFIG INS TIMEOUT 60\n"
                                 "CONFIG COM1 460800";
    ReceiverConfig rc = parseConfig(defaults);
    CHECK(rc.hasImuToAnt && rc.hasInsSol && rc.square);
    std::vector<std::string> cmds;
    CHECK(decide(rc, arm, cmds) == Decision::Write);
    CHECK(cmds.size() == 2 && cmds[0] == imuToAntCommand(arm) && cmds[1] == insSolCommand(arm));

    // Already within 1 cm, both: nothing to send. One of the two out: both are sent.
    rc = parseConfig("CONFIG IMUTOANT OFFSET -0.405 0.80 1.495 0.03 0.03 0.03\nCONFIG INSSOL OFFSET -0.40 0.79 1.50");
    CHECK(decide(rc, arm, cmds) == Decision::AlreadySet && cmds.empty());
    rc = parseConfig("CONFIG IMUTOANT OFFSET -0.40 0.80 1.50 0.03 0.03 0.03\nCONFIG INSSOL OFFSET 0.00 0.00 0.00");
    CHECK(decide(rc, arm, cmds) == Decision::Write && cmds.size() == 2);
    rc = parseConfig("CONFIG IMUTOANT OFFSET -0.40 0.80 1.50 0.03 0.03 0.03");  // no INSSOL listed
    CHECK(decide(rc, arm, cmds) == Decision::Write);

    // A UM982 (no IMUTOANT), a receiver mounted turned, a lever arm the receiver cannot take.
    CHECK(decide(parseConfig("CONFIG COM1 460800\nCONFIG HEADING FIXLENGTH"), arm, cmds) == Decision::NoIns && cmds.empty());
    CHECK(decide(parseConfig("CONFIG INS ANGLE 0 9000 18000\nCONFIG IMUTOANT OFFSET 0 0 0 0.01 0.01 0.01"), arm, cmds) == Decision::Rotated);
    CHECK(parseConfig("CONFIG INS ANGLE 36000 0 0\nCONFIG IMUTOANT OFFSET 0 0 0").square);  // a full turn is square
    CHECK(decide(rc, Vec{0, 150, 0}, cmds) == Decision::OutOfRange && cmds.empty());

    // The job: when to talk to the receiver.
    {
        LeverArmJob job;
        LeverArmJob::Conditions c;
        c.receiverIns = true;
        CHECK(!job.due(c, 0));  // no geometry yet
        job.onGeometry(g);
        CHECK(job.due(c, 0) && near(job.want().y, 0.80));
        job.finished(true, Decision::Write, 0);
        CHECK(!job.due(c, 1) && job.message().find("saved on the receiver") != std::string::npos);
        // The same geometry again (the app resends it), or with only the wheelbase changed: no new check.
        g.wheelbaseCm = 300;
        job.onGeometry(g);
        CHECK(!job.due(c, 2));
        // The antenna moved: checked again, but not while steering or moving.
        g.antennaPivotCm = 130;
        job.onGeometry(g);
        c.steering = true;
        CHECK(!job.due(c, 3) && job.message().find("waiting for steering off") != std::string::npos);
        c.steering = false;
        c.speedKph = 5;
        CHECK(!job.due(c, 4));
        c.speedKph = 0.5f;
        CHECK(job.due(c, 5) && near(job.want().y, 0.90));
        // The receiver did not answer: again after a minute, not before.
        job.finished(false, Decision::Write, 1000);
        CHECK(!job.due(c, 1000 + LeverArmJob::kRetryMs - 1) && job.due(c, 1000 + LeverArmJob::kRetryMs));
        job.finished(true, Decision::AlreadySet, 70000);
        CHECK(!job.due(c, 70001));
        // The GPS page wrote the receiver's settings: checked again.
        job.recheck();
        CHECK(job.due(c, 70002));
    }
    {
        // No IMU position entered, no INS, or an external IMU: never due, and the reason is given.
        LeverArmJob job;
        LeverArmJob::Conditions c;
        c.receiverIns = true;
        pgn::VehicleGeometry none = g;
        none.imuEntered = false;
        job.onGeometry(none);
        CHECK(!job.due(c, 0) && job.message().find("No IMU position") != std::string::npos);
        job.onGeometry(g);  // the position is entered later
        c.receiverIns = false;
        CHECK(!job.due(c, 0) && job.message().find("no INS") != std::string::npos);
        c.receiverIns = true;
        c.externalImu = true;
        CHECK(!job.due(c, 0) && job.message().find("external IMU") != std::string::npos);
        c.externalImu = false;
        CHECK(job.due(c, 0));
    }

    return finish("ins_setup");
}
