// Autosteer: every path that stops the motor, then the controller itself.
#include "aow/steer.h"

#include <cstdlib>

#include "check.h"

using namespace aow;
using namespace aow::steer;

struct FakeClock : hal::Clock {
    uint64_t us = 10'000'000;
    uint64_t nowUs() override { return us; }
};

// The app at 100 Hz and the control loop at 100 Hz, in lock step.
struct Rig {
    FakeClock clock;
    Autosteer a;
    Inputs in;
    float speed = 5.0f, target = 0.0f;
    bool guidance = true, sending = true;
    Output out;

    explicit Rig(const Settings& s = Settings()) : a(clock, s) {}

    static uint16_t countsFor(float deg, uint8_t perDeg = 30) { return static_cast<uint16_t>(2048 + deg * perDeg); }

    void frame(bool g) {
        pgn::SteerCommand c;
        c.speedKmh = speed;
        c.status = g ? 0x01 : 0x00;
        c.targetAngleDeg = target;
        a.onSteerData(c);
    }
    // One 10 ms step: a PGN 254 (if the app is sending), then the control tick.
    Output step() {
        clock.us += 10'000;
        if (sending) frame(guidance);
        out = a.tick(in);
        return out;
    }
    void run(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 10) step(); }
    // Guidance off then on: the app arms the module.
    void engage() {
        guidance = false;
        step();
        guidance = true;
        step();
    }
};

static Settings noSoftStart() {
    Settings s;
    s.steer.softStartDurationMs = 0;
    return s;
}

int main() {
    // ---------------------------------------------------------------- stop paths

    // No PGN 254 ever: nothing runs, however long we wait.
    {
        Rig r;
        r.sending = false;
        r.run(3000);
        CHECK(!r.out.enable && !r.a.shouldSteer() && !r.a.armed());
    }

    // Engaging works at all (the baseline every other stop test departs from).
    {
        Rig r;
        r.engage();
        CHECK(r.a.armed() && r.a.shouldSteer() && r.out.enable);
    }

    // Steer timeout: the app goes quiet. Still steering at 2.0 s, off just after.
    {
        Rig r;
        r.engage();
        r.run(500);
        r.sending = false;
        r.run(2000);
        CHECK(r.out.enable);  // exactly 2000 ms since the last frame: the firmware uses '>'
        r.step();
        CHECK(!r.out.enable && r.a.motorState() == MotorState::Disabled);
        CHECK(r.a.armed());  // the timeout stops the motor; it does not disarm
        // The app comes back: steering resumes, through a fresh soft start.
        r.sending = true;
        r.step();
        CHECK(r.out.enable && r.a.motorState() == MotorState::SoftStart);
    }

    // Guidance off in the app: the motor stops on that tick; disarm follows after 300 ms.
    {
        Rig r;
        r.engage();
        r.run(200);
        r.guidance = false;
        r.step();
        CHECK(!r.out.enable && r.a.armed());
        r.run(300);
        CHECK(r.a.armed());
        r.run(20);
        CHECK(!r.a.armed());
        // Guidance on again re-arms (the app's steer button).
        r.guidance = true;
        r.step();
        CHECK(r.a.armed() && r.out.enable);
    }

    // Too slow: minimum speed 3 (0.3 km/h); the firmware needs strictly more.
    {
        Rig r;
        r.speed = 0.3f;
        r.engage();
        CHECK(r.a.armed() && !r.out.enable);
        r.speed = 0.31f;
        r.step();
        CHECK(r.out.enable);
        r.speed = 0.0f;
        r.step();
        CHECK(!r.out.enable);
    }

    // Steer switch mode: opening the switch disarms (after the 50 ms debounce).
    {
        Settings s;
        s.steer.steerSwitch = true;
        Rig r(s);
        r.in.steerSwitch = true;
        r.run(60);
        CHECK(r.a.armed() && r.out.enable);
        r.in.steerSwitch = false;
        r.run(40);
        CHECK(r.a.armed() && r.out.enable);  // still within the debounce
        r.run(20);
        CHECK(!r.a.armed() && !r.out.enable);
        // Guidance off with the switch closed does not disarm (the switch holds it).
        r.in.steerSwitch = true;
        r.run(60);
        r.guidance = false;
        r.run(500);
        CHECK(r.a.armed() && !r.out.enable);
    }

    // A contact bounce shorter than the debounce changes nothing.
    {
        Settings s;
        s.steer.steerSwitch = true;
        Rig r(s);
        r.in.steerSwitch = true;
        r.run(100);
        r.in.steerSwitch = false;
        r.step();
        r.step();
        r.in.steerSwitch = true;
        r.run(200);
        CHECK(r.a.armed() && r.out.enable);
    }

    // Steer button mode: each press toggles.
    {
        Settings s;
        s.steer.steerButton = true;
        Rig r(s);
        r.guidance = true;
        r.run(100);
        CHECK(!r.a.armed());
        r.in.steerSwitch = true;  // press
        r.run(60);
        CHECK(r.a.armed() && r.out.enable);
        r.in.steerSwitch = false;  // release: nothing
        r.run(60);
        CHECK(r.a.armed());
        r.in.steerSwitch = true;  // second press: off
        r.run(60);
        CHECK(!r.a.armed() && !r.out.enable);
    }

    // Current kickout: over threshold + 10 % for a full second stops and disarms, and the
    // motor stays off for the 2 s cooldown even if the operator re-arms at once.
    {
        Settings s;
        s.steer.currentSensor = true;
        s.steer.currentThreshold = 100;  // 100/255 of 1680 counts = 658, +10 % = 723
        Rig r(s);
        r.engage();
        r.in.currentVolts = 1.0f;  // 1.0 V -> 1164 counts
        r.run(30);                 // the 50 ms average climbs past 723 on the 4th sample
        r.run(990);
        CHECK(r.out.enable && r.a.armed());  // not yet a full second over
        r.run(20);
        CHECK(!r.out.enable && !r.a.armed());
        // The operator re-arms straight away (guidance off, on): armed, but no motor yet.
        r.in.currentVolts = 0.0f;
        r.engage();
        CHECK(r.a.armed() && !r.out.enable && !r.a.shouldSteer());
        r.run(1900);
        CHECK(!r.out.enable);
        r.run(200);
        CHECK(r.out.enable);
    }

    // A current spike shorter than a second does not kick out.
    {
        Settings s;
        s.steer.currentSensor = true;
        s.steer.currentThreshold = 100;
        Rig r(s);
        r.engage();
        r.in.currentVolts = 1.0f;
        r.run(800);
        r.in.currentVolts = 0.0f;
        r.run(100);
        r.in.currentVolts = 1.0f;
        r.run(800);
        CHECK(r.out.enable && r.a.armed());
    }

    // Current sensor off in PGN 251: no current kickout at all.
    {
        Rig r;
        r.engage();
        r.in.currentVolts = 4.0f;
        r.run(3000);
        CHECK(r.out.enable && r.a.armed() && r.a.kickout() == Kickout::None);
    }

    // Several PGN 254 frames in one tick: a toggle followed by an unchanged frame still counts.
    // (The firmware kept one "changed" flag per frame, so the second frame erased the first.)
    {
        Rig r;
        r.guidance = false;
        r.run(50);
        r.frame(true);  // off -> on
        r.frame(true);  // on -> on, same tick
        r.guidance = true;
        r.step();       // and the step's own frame, on
        CHECK(r.a.armed());
    }

    // ---------------------------------------------------------------- the controller

    // P term plus minimum PWM, limited to high PWM; motor direction; Ackermann on the left.
    {
        Settings s = noSoftStart();  // Kp 40, min 10, high 255, 30 counts per degree
        Rig r(s);
        r.in.wasCounts = Rig::countsFor(2.0f);  // actual +2, target 0: error +2 -> 80 + 10
        r.engage();
        CHECK(r.out.enable && r.out.pwm == 90);
        r.in.wasCounts = Rig::countsFor(10.0f);  // 400 + 10 -> limited to 255
        r.step();
        CHECK(r.out.pwm == 255);
        r.target = 10.0f;  // on target: no drive
        r.step();
        CHECK(r.out.enable && r.out.pwm == 0);

        s.gains.ackermanFix = 0.8f;
        s.steer.motorDriveDirection = true;
        r.a.setSettings(s);
        r.target = 0;
        r.in.wasCounts = Rig::countsFor(-2.0f);  // -2 * 0.8 = -1.6 -> -64 - 10 = -74, reversed
        r.run(600);                              // past any ramp a reversal started
        CHECK(r.out.pwm == 74);
        CHECK(r.a.actualAngleDeg() > -1.61f && r.a.actualAngleDeg() < -1.59f);
        CHECK(r.a.status().pwmDisplay == 74);
    }

    // Soft start: ramps from min PWM to the full command over the set time.
    {
        Settings s;
        s.steer.softStartDurationMs = 500;
        Rig r(s);
        r.in.wasCounts = Rig::countsFor(2.0f);  // full command 90
        r.engage();
        CHECK(r.a.motorState() == MotorState::SoftStart && r.out.pwm == 10);  // 0 % -> min PWM
        r.run(250);
        CHECK(r.out.pwm >= 44 && r.out.pwm <= 46);  // about half
        r.run(260);
        CHECK(r.a.motorState() == MotorState::Normal && r.out.pwm == 90);

        // A reversal past min + 20 ramps again from min PWM, the other way.
        r.in.wasCounts = Rig::countsFor(-2.0f);  // -90
        r.step();
        CHECK(r.a.motorState() == MotorState::SoftStart && r.out.pwm == -10);
    }

    // High PWM 0 is invalid: the firmware keeps the motor enabled at zero.
    {
        Settings s = noSoftStart();
        s.gains.highPwm = 0;
        Rig r(s);
        r.in.wasCounts = Rig::countsFor(5.0f);
        r.engage();
        CHECK(r.out.enable && r.out.pwm == 0);
    }

    // ---------------------------------------------------------------- settings from the app

    {
        Rig r;
        // PGN 251: invert WAS, motor reversed, switch mode; min speed 5; current sensor on,
        // threshold 150.
        const uint8_t cfg[] = {0x01 | 0x04 | 0x20, 150, 5, 0x04, 0, 0, 0, 0};
        CHECK(r.a.onSteerConfig(cfg, sizeof cfg));
        const Settings& s = r.a.settings();
        CHECK(s.steer.invertWas && s.steer.motorDriveDirection && s.steer.steerSwitch && !s.steer.steerButton);
        CHECK(s.steer.minSpeed == 5 && s.steer.currentSensor && s.steer.currentThreshold == 150);
        CHECK(s.steer.motorDriverConfig == 0x04);
        CHECK(!r.a.onSteerConfig(cfg, sizeof cfg));  // same again: unchanged
        CHECK(!r.a.onSteerConfig(cfg, 3));           // too short: ignored

        // PGN 252: Kp 60, high 200, (low 99, unused), min 20, 45 counts/deg, offset -100, Ackermann 0.90.
        const uint8_t gains[] = {60, 200, 99, 20, 45, 0x9C, 0xFF, 90};
        CHECK(r.a.onSteerSettings(gains, sizeof gains));
        const Settings& g = r.a.settings();
        CHECK(g.gains.kp == 60 && g.gains.highPwm == 200 && g.gains.minPwm == 20);
        CHECK(g.gains.steerSensorCounts == 45 && g.gains.wasOffset == -100);
        CHECK(g.gains.ackermanFix > 0.899f && g.gains.ackermanFix < 0.901f);
        CHECK(!r.a.onSteerSettings(gains, 7));

        // WAS scaling with those: (counts - 2048 - offset) / counts-per-degree, inverted.
        CHECK(wasAngle(2048 - 100 + 45 * 3, g) == -3.0f);
    }

    // ---------------------------------------------------------------- what the app sees

    {
        Settings s;
        s.steer.currentSensor = true;
        Rig r(s);
        r.in.workSwitch = true;
        r.in.remoteSwitch = true;
        r.in.currentVolts = 1.0f;
        r.run(100);
        pgn::SteerStatus st = r.a.status();
        CHECK(st.workSwitchOn && st.remoteOrKickout && !st.steerArmed);
        CHECK(r.a.turnSensorReading() == 176);  // 1164 counts of 1680 -> 176/255

        s.misc.invertWorkSwitch = true;
        r.a.setSettings(s);
        CHECK(!r.a.status().workSwitchOn);

        s.steer.currentSensor = false;
        r.a.setSettings(s);
        CHECK(r.a.turnSensorReading() == 0);
    }

    // Debouncer on its own.
    {
        Debouncer d;
        CHECK(!d.update(true, 0) && !d.state());
        CHECK(!d.update(true, 49) && !d.state());
        CHECK(d.update(true, 50) && d.state());
        CHECK(!d.update(true, 60));
    }

    return finish("steer");
}
