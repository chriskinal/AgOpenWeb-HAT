// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
//
// The ported tractor CAN driver against known frames: Keya, legacy Valtra and Fendt, and the
// JSON protocol engine with the firmware's own Valtra/MF brand file.
#include "aow/can/TractorCANDriver.h"

#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "check.h"

#ifndef AOW_SOURCE_DIR
#define AOW_SOURCE_DIR "."
#endif

struct FakeClock : aow::hal::Clock {
    uint64_t us = 5'000'000;
    uint64_t nowUs() override { return us; }
};

struct FakeBus : aow::hal::CanBus {
    std::deque<aow::hal::CanFrame> in;
    std::vector<aow::hal::CanFrame> out;
    bool read(aow::hal::CanFrame& f) override {
        if (in.empty()) return false;
        f = in.front();
        in.pop_front();
        return true;
    }
    bool write(const aow::hal::CanFrame& f) override {
        out.push_back(f);
        return true;
    }
};

static aow::hal::CanFrame frame(uint32_t id, std::vector<uint8_t> data) {
    aow::hal::CanFrame f;
    f.id = id;
    f.extended = id > 0x7FF;
    f.len = static_cast<uint8_t>(data.size());
    for (size_t i = 0; i < data.size(); ++i) f.data[i] = data[i];
    return f;
}

static bool is(const aow::hal::CanFrame& f, uint32_t id, std::vector<uint8_t> data) {
    if (f.id != id || f.len != data.size()) return false;
    for (size_t i = 0; i < data.size(); ++i)
        if (f.data[i] != data[i]) return false;
    return true;
}

static std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

struct Rig {
    FakeClock clock;
    FakeBus bus[3];
    std::vector<std::string> popups;
    int mirrored = 0;
    Rig() {
        CanCompatHooks h;
        h.clock = &clock;
        for (int i = 0; i < 3; ++i) h.bus[i] = &bus[i];
        h.popup = [this](const char* m, uint8_t, uint8_t) { popups.push_back(m); };
        h.mirror = [this](uint8_t, const CAN_message_t&, bool) { ++mirrored; };
        canCompatSetup(h);
    }
};

int main() {
    // --- Keya CAN motor: brand GENERIC, KEYA function on CAN3.
    {
        Rig r;
        TractorCANDriver d;
        CANSteerConfig c;
        c.brand = 6;
        c.can3Function = static_cast<uint8_t>(CANFunction::KEYA);
        d.configure(c, "");
        d.init();
        CHECK(std::string(d.getTypeName()) == "Keya CAN");

        // Heartbeat: ready.
        CHECK(!d.isHeartbeatValid());
        r.bus[2].in.push_back(frame(0x07000001, {0, 10, 0, 50, 0, 3, 0, 0}));
        d.process();
        CHECK(d.isHeartbeatValid() && d.isValveReady() && d.getActualRPM() == 50);

        // Enabled at full PWM: enable and speed frames alternate; 255 PWM = 100 RPM = 1000.
        r.bus[2].out.clear();
        d.enable(true);
        d.setPWM(255);
        d.process();
        d.process();
        CHECK(r.bus[2].out.size() == 2);
        CHECK(is(r.bus[2].out[0], 0x06000001, {0x23, 0x0D, 0x20, 0x01, 0, 0, 0, 0}));
        CHECK(is(r.bus[2].out[1], 0x06000001, {0x23, 0x00, 0x20, 0x01, 0x03, 0xE8, 0, 0}));
        d.setPWM(-255);  // -1000 = 0xFFFFFC18, in the Keya's word order
        d.process();
        d.process();
        CHECK(is(r.bus[2].out[3], 0x06000001, {0x23, 0x00, 0x20, 0x01, 0xFC, 0x18, 0xFF, 0xFF}));

        // Disabled: disable and zero-speed frames alternate.
        r.bus[2].out.clear();
        d.enable(false);
        d.process();
        d.process();
        CHECK(is(r.bus[2].out[0], 0x06000001, {0x23, 0x0C, 0x20, 0x01, 0, 0, 0, 0}) ||
              is(r.bus[2].out[1], 0x06000001, {0x23, 0x0C, 0x20, 0x01, 0, 0, 0, 0}));

        // No heartbeat for 250 ms: not ready.
        r.clock.us += 260'000;
        d.process();
        CHECK(!d.isValveReady() && !d.isHeartbeatValid());
        CHECK(r.mirrored > 0);  // frames reach the GVRET mirror
    }

    // --- Valtra/MF, legacy code (no JSON): steering on CAN3.
    {
        Rig r;
        TractorCANDriver d;
        CANSteerConfig c;
        c.brand = 9;
        c.can3Function = static_cast<uint8_t>(CANFunction::STEERING);
        d.configure(c, "");
        d.init();
        CHECK(d.providesCANWheelAngle());
        r.bus[2].in.push_back(frame(0x0CAC1C13, {0x10, 0x27, 1, 0, 0, 0, 0, 0}));  // curve 10000, ready
        d.process();
        CHECK(d.isValveReady() && d.getActualCurve() == 10000 && d.getCurveScale() == 100.0f);
        r.bus[2].out.clear();
        d.enable(true);
        d.setPWM(50);  // 50 x 128 = 6400 = 0x1900
        d.process();
        CHECK(r.bus[2].out.size() == 1 && is(r.bus[2].out[0], 0x0CAD131C, {0x00, 0x19, 253, 0, 0, 0, 0, 0}));
        d.enable(false);
        d.process();
        CHECK(is(r.bus[2].out[1], 0x0CAD131C, {0, 0, 252, 0, 0, 0, 0, 0}));
        r.bus[2].in.push_back(frame(0x0CAC1C13, {0x10, 0x27, 0, 0, 0, 0, 0, 0}));  // valve not ready
        d.process();
        CHECK(!d.isValveReady());
    }

    // --- Valtra/MF through the JSON engine, with the firmware's brand file. Buttons on CAN1.
    {
        Rig r;
        TractorCANDriver d;
        CANSteerConfig c;
        c.brand = 9;
        c.can3Function = static_cast<uint8_t>(CANFunction::STEERING);
        c.can1Function = static_cast<uint8_t>(CANFunction::BUTTONS);
        const std::string json = readFile(AOW_SOURCE_DIR "/deploy/can-config/Valtra_Massey_Ferguson.json");
        CHECK(json.size() > 1000);
        d.configure(c, json);
        d.init();
        CHECK(d.providesCANWheelAngle() == false);  // the engine says so only once valve data arrives

        r.bus[2].in.push_back(frame(0x0CAC1C13, {0xE8, 0x03, 1, 0, 0, 0, 0, 0}));  // curve 1000, ready
        d.process();
        CHECK(d.isValveReady() && d.isValveDataReceived() && d.providesCANWheelAngle() && d.getActualCurve() == 1000);

        // Command: curve = actual - PWM = 900 = 0x0384, template 0,255,... with intent 253 at byte 2.
        r.bus[2].out.clear();
        d.enable(true);
        d.setPWM(100);
        d.process();
        CHECK(r.bus[2].out.size() == 1 && is(r.bus[2].out[0], 0x0CAD131C, {0x84, 0x03, 253, 255, 255, 255, 255, 0}));
        d.enable(false);
        d.process();
        CHECK(is(r.bus[2].out[1], 0x0CAD131C, {0xE8, 0x03, 252, 255, 255, 255, 255, 0}));  // disabled: PWM 0, curve 1000

        // K_Bus engage button (S-Series: 0x0CFF2621 byte 3 & 4): one event per press.
        r.bus[0].in.push_back(frame(0x0CFF2621, {0, 0, 0, 0x04, 0, 0, 0, 0}));
        d.process();
        CHECK(d.checkEngageEvent() && std::string(d.getEngageLabel()) == "S-series Steering engage button");
        r.bus[0].in.push_back(frame(0x0CFF2621, {0, 0, 0, 0x04, 0, 0, 0, 0}));  // still held
        d.process();
        CHECK(!d.checkEngageEvent());
        r.bus[0].in.push_back(frame(0x0CFF2621, {0, 0, 0, 0x00, 0, 0, 0, 0}));  // released
        r.bus[0].in.push_back(frame(0x0CFF2621, {0, 0, 0, 0x04, 0, 0, 0, 0}));  // pressed again
        d.process();
        CHECK(d.checkEngageEvent());
    }

    // --- JSON kickout (MF 7600 file): valve status leaves its ready values while steering.
    {
        Rig r;
        TractorCANDriver d;
        CANSteerConfig c;
        c.brand = 9;
        c.can3Function = static_cast<uint8_t>(CANFunction::STEERING);
        d.configure(c, readFile(AOW_SOURCE_DIR "/deploy/can-config/Massey_Ferguson_7600_VBus.json"));
        d.init();
        d.enable(true);
        r.bus[2].in.push_back(frame(0x0CAC1C13, {0, 0, 20, 0, 0, 0, 0, 0}));  // 20: system ready
        d.process();
        CHECK(!d.isKickoutDetected() && d.getStatus().enabled);
        r.bus[2].in.push_back(frame(0x0CAC1C13, {0, 0, 80, 0, 0, 0, 0, 0}));  // 80: manual steering
        d.process();
        CHECK(d.isKickoutDetected() && !d.getStatus().enabled);  // the driver drops its own enable
        CHECK(r.popups.size() == 1 && r.popups[0].find("Manual steering detected!") != std::string::npos);
        d.resetKickoutDetected();
        CHECK(!d.isKickoutDetected());

        // Not steering (onlyWhenEngaged): the same drop is ignored.
        r.clock.us += 1'000'000;
        r.bus[2].in.push_back(frame(0x0CAC1C13, {0, 0, 20, 0, 0, 0, 0, 0}));
        r.bus[2].in.push_back(frame(0x0CAC1C13, {0, 0, 80, 0, 0, 0, 0, 0}));
        d.process();
        CHECK(!d.isKickoutDetected());
    }

    // --- Fendt, legacy code: 6-byte command.
    {
        Rig r;
        TractorCANDriver d;
        CANSteerConfig c;
        c.brand = 4;
        c.can2Function = static_cast<uint8_t>(CANFunction::STEERING);
        d.configure(c, "");
        d.init();
        r.bus[1].in.push_back(frame(0x0CEF2CF0, {0x7D, 0x80, 1, 0, 0, 0, 0, 0}));  // not "len 3, byte 2 = 0": ready
        d.process();
        CHECK(d.isValveReady());
        r.bus[1].out.clear();
        d.enable(true);
        d.setPWM(0);
        d.process();
        // curve = PWM - 32128 = -32128 = 0x8280
        CHECK(r.bus[1].out.size() == 1 && is(r.bus[1].out[0], 0x0CEFF02C, {0x05, 0x09, 0x03, 0x0A, 0x82, 0x80}));
    }

    return finish("can");
}
