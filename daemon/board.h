// The hardware the control loop reads and drives. One implementation per board: the HAT
// (boards/hat, when it exists) and a simulator for a CM4 with no HAT.
#pragma once

#include <vector>

#include "aow/indicators.h"
#include "aow/motor.h"
#include "aow/settings.h"
#include "aow/steer.h"

namespace aow {

struct Board {
    virtual ~Board() = default;
    virtual const char* name() const = 0;
    virtual steer::Inputs read(const Settings& s) = 0;
    // enable false must leave the motor unpowered, whatever pwm says.
    virtual void drive(const steer::Output& out) = 0;
    // The four status LEDs, 10 times a second.
    virtual void showLeds(const indicators::Rgb (&leds)[4]) { (void)leds; }
    // The piezo. Must not block: the HAT driver plays them in the background.
    virtual void playTones(const std::vector<indicators::Tone>& tones) { (void)tones; }
    // The hardware watchdog (the HAT's STWD100, 1.6 s). Enabled once the control loop runs,
    // kicked from it; a stalled daemon or kernel then resets the CM4, as the Teensy's did.
    virtual void watchdog(bool enable) { (void)enable; }
    virtual void kickWatchdog() {}
};

// No hardware. The wheel follows the motor: full PWM turns it 30 degrees a second, within
// +-40 degrees, and the WAS reads that angle back through the app's calibration. Switches are
// open and no current flows. Lets the whole loop (app, controller, PGN 253) run on a bare CM4.
class SimBoard : public Board {
public:
    const char* name() const override { return "sim"; }

    steer::Inputs read(const Settings& s) override {
        steer::Inputs in;
        const float perDeg = s.gains.steerSensorCounts ? s.gains.steerSensorCounts : 30;
        float counts = 2048.0f + s.gains.wasOffset + (s.steer.invertWas ? -angle_ : angle_) * perDeg;
        in.wasCounts = static_cast<uint16_t>(counts < 0 ? 0 : counts > 4095 ? 4095 : counts);
        return in;
    }

    void drive(const steer::Output& out) override {
        // Called at 100 Hz. The controller drives towards (actual - target) > 0 with positive
        // PWM, so positive PWM must reduce the angle.
        const int pwm = out.enable ? out.pwm : 0;
        angle_ -= pwm / 255.0f * 30.0f * 0.01f;
        if (angle_ > 40) angle_ = 40;
        if (angle_ < -40) angle_ = -40;
    }

private:
    float angle_ = 0;
};

// The PWM motor: the board's own output (the HAT's PWM + DIR + EN, or the simulator's wheel).
class PwmMotor : public motor::Motor {
public:
    explicit PwmMotor(Board& board) : board_(board) {}
    const char* name() const override { return "PWM"; }
    void command(bool enable, int16_t pwm) override { board_.drive({enable, pwm}); }

private:
    Board& board_;
};

}  // namespace aow
