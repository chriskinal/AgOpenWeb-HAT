// The autosteer with CAN and Keya motors: readiness gating, the app-button handshake, motor
// errors, CAN engage buttons, CAN kickout, CAN wheel angle. Plus the Keya serial protocol.
#include <deque>
#include <string>
#include <vector>

#include "aow/motor.h"
#include "aow/steer.h"
#include "check.h"

using namespace aow;
using namespace aow::steer;

struct FakeClock : hal::Clock {
    uint64_t us = 10'000'000;
    uint64_t nowUs() override { return us; }
};

struct FakeMotor : motor::Motor {
    bool can = true, isReady = true, error = false, kick = false, owns = true;
    int engagePresses = 0;
    bool haveAngle = false;
    float angle = 0;
    bool enabled = false;
    int16_t pwm = 0;
    const char* name() const override { return "fake"; }
    void command(bool e, int16_t p) override { enabled = e; pwm = p; }
    bool hasError() const override { return error; }
    bool ownsKickout() const override { return owns; }
    bool isTractorCan() const override { return can; }
    bool ready() const override { return isReady; }
    const char* notReadyMessage() const override { return "valve not ready"; }
    bool takeEngageEvent(std::string& label) override {
        if (!engagePresses) return false;
        --engagePresses;
        label = "CAN button";
        return true;
    }
    bool canKickout() const override { return kick; }
    void clearCanKickout() override { kick = false; }
    bool canWheelAngle(float& d) const override { if (haveAngle) d = angle; return haveAngle; }
};

struct Rig {
    FakeClock clock;
    FakeMotor m;
    Autosteer a;
    Inputs in;
    bool guidance = true;
    Output out;
    std::vector<std::string> popups;

    explicit Rig(const Settings& s = Settings()) : a(clock, s) {
        a.setMotor(&m);
        a.setPopup([this](const char* msg, uint8_t, uint8_t) { popups.push_back(msg); });
    }
    Output step() {
        clock.us += 10'000;
        pgn::SteerCommand c;
        c.speedKmh = 5;
        c.status = guidance ? 1 : 0;
        a.onSteerData(c);
        out = a.tick(in);
        m.command(out.enable, out.pwm);
        return out;
    }
    void run(uint32_t ms) { for (uint32_t t = 0; t < ms; t += 10) step(); }
    void engage() { guidance = false; step(); guidance = true; step(); }
};

// Keya over RS-232: a fake serial line.
struct FakeSerial : hal::SerialPort {
    std::deque<uint8_t> rx;
    std::vector<uint8_t> tx;
    std::vector<uint8_t> replyOnWrite;  // what the device answers, once written to
    int read(uint8_t* buf, size_t cap) override {
        size_t n = 0;
        while (n < cap && !rx.empty()) { buf[n++] = rx.front(); rx.pop_front(); }
        return static_cast<int>(n);
    }
    int write(const uint8_t* buf, size_t len) override {
        tx.insert(tx.end(), buf, buf + len);
        rx.insert(rx.end(), replyOnWrite.begin(), replyOnWrite.end());
        replyOnWrite.clear();
        return static_cast<int>(len);
    }
};

int main() {
    // App steer button with the CAN valve not ready: a popup, armed for 200 ms so the app sees
    // it, the motor never runs, then disarmed.
    {
        Rig r;
        r.m.isReady = false;
        r.engage();
        CHECK(r.popups.size() == 1 && r.popups[0] == "valve not ready");
        CHECK(r.a.armed() && r.a.status().steerArmed && !r.out.enable && !r.m.enabled);
        r.run(180);
        CHECK(r.a.armed() && !r.out.enable && r.popups.size() == 1);
        r.run(30);
        CHECK(!r.a.armed() && !r.out.enable);
    }

    // Valve ready: arming and steering as normal.
    {
        Rig r;
        r.engage();
        CHECK(r.a.armed() && r.out.enable && r.popups.empty());
    }

    // Valve drops out while steering: disarm, stop, popup; then the 2 s cooldown.
    {
        Rig r;
        r.engage();
        r.run(100);
        r.m.isReady = false;
        r.step();
        CHECK(!r.a.armed() && !r.out.enable && r.popups.size() == 1);
        r.m.isReady = true;
        r.engage();
        CHECK(r.a.armed() && !r.out.enable);  // cooldown
        r.run(2000);
        CHECK(r.out.enable);
    }

    // Motor error while steering: stop and cool down. The same error while disarmed: nothing.
    {
        Rig r;
        r.engage();
        r.m.error = true;
        r.step();
        CHECK(!r.a.armed() && !r.out.enable);
        Rig q;
        q.m.error = true;
        q.guidance = true;
        q.run(100);
        CHECK(!q.a.armed() && q.popups.empty());
    }

    // Button mode: a CAN engage button toggles like the physical one; blocked while not ready.
    {
        Settings s;
        s.steer.steerButton = true;
        Rig r(s);
        r.m.engagePresses = 1;
        r.step();
        CHECK(r.a.armed() && r.out.enable);
        r.m.engagePresses = 1;
        r.step();
        CHECK(!r.a.armed());
        r.m.isReady = false;
        r.m.engagePresses = 1;
        r.step();
        CHECK(!r.a.armed() && r.popups.size() == 1);
    }

    // Switch mode: switching on with the valve not ready is refused (one popup), and takes
    // effect once the valve is ready if the switch is still on.
    {
        Settings s;
        s.steer.steerSwitch = true;
        Rig r(s);
        r.m.isReady = false;
        r.in.steerSwitch = true;
        r.run(300);
        CHECK(!r.a.armed() && r.popups.size() == 1);
        r.m.isReady = true;
        r.step();
        CHECK(r.a.armed() && r.out.enable);
    }

    // CAN kickout: disarm and stop; reported once, then clear. The current sensor is not used
    // with a motor that reports its own kickouts.
    {
        Settings s;
        s.steer.currentSensor = true;
        Rig r(s);
        r.engage();
        r.in.currentVolts = 4.0f;  // far over any threshold
        r.run(1500);
        CHECK(r.a.armed() && r.out.enable && r.a.kickout() == Kickout::None);
        r.m.kick = true;
        r.step();
        CHECK(!r.a.armed() && !r.out.enable && r.a.kickout() == Kickout::Can && !r.m.kick);
        r.step();
        CHECK(r.a.kickout() == Kickout::None);
    }

    // CAN wheel angle replaces the WAS.
    {
        Rig r;
        r.in.wasCounts = 2048 + 30 * 7;  // the WAS says 7 degrees
        r.m.haveAngle = true;
        r.m.angle = -3.5f;
        r.step();
        CHECK(r.a.wasAngleDeg() == -3.5f);
        r.m.haveAngle = false;
        r.step();
        CHECK(r.a.wasAngleDeg() == 7.0f);
    }

    // A PWM motor (not tractor CAN) is never gated.
    {
        Rig r;
        r.m.can = false;
        r.m.isReady = false;
        r.engage();
        CHECK(r.a.armed() && r.out.enable && r.popups.empty());
    }

    // ---- Keya serial protocol
    {
        FakeClock clock;
        FakeSerial port;

        // Probe: E2 00 00 E2 out, E2 ... back.
        port.rx = {0x55, 0x55};  // stale bytes: the probe must drop them first
        port.replyOnWrite = {0xE2, 0x01, 0x02, 0x03, 0x04};
        CHECK(motor::KeyaSerialMotor::probe(port, clock));
        CHECK(port.tx == std::vector<uint8_t>({0xE2, 0x00, 0x00, 0xE2}));

        port.tx.clear();
        motor::KeyaSerialMotor k(port, clock);
        CHECK(k.hasError());  // nothing heard yet

        // Full PWM: AD, speed 1000 = 0x03E8, checksum.
        k.command(true, 255);
        clock.us += 20'000;
        k.process();
        CHECK(port.tx == std::vector<uint8_t>({0xAD, 0x03, 0xE8, static_cast<uint8_t>(0xAD + 0x03 + 0xE8)}));

        // A 15-byte status: AC ... AD rpm current volts ... AE error temp.
        const std::vector<uint8_t> status = {0xAC, 0, 0, 0, 0, 0xAD, 100, 25, 12, 0, 0xAE, 0, 0, 30, 0};
        port.rx.assign(status.begin(), status.end());
        clock.us += 20'000;
        k.process();
        CHECK(!k.hasError() && k.actualRpm() == 100 && k.currentAmps() > 2.49f && k.currentAmps() < 2.51f);

        // Disabled: AC 00 00 AC.
        port.tx.clear();
        k.command(false, 255);
        clock.us += 20'000;
        k.process();
        CHECK(port.tx == std::vector<uint8_t>({0xAC, 0x00, 0x00, 0xAC}));

        // Slip: commanded 100 RPM, motor reports 10, for over 200 ms -> error.
        k.command(true, 255);
        for (int i = 0; i < 15; ++i) {
            const std::vector<uint8_t> slow = {0xAC, 0, 0, 0, 0, 0xAD, 10, 25, 12, 0, 0xAE, 0, 0, 30, 0};
            port.rx.assign(slow.begin(), slow.end());
            clock.us += 20'000;
            k.process();
        }
        CHECK(k.hasError());

        // Silence for over a second -> error, even without slip.
        k.command(false, 0);
        port.rx.assign(status.begin(), status.end());
        clock.us += 20'000;
        k.process();
        clock.us += 1'100'000;
        k.process();
        CHECK(k.hasError());
    }

    return finish("steer_motor");
}
