#include "aow/settings.h"

#include <cstdio>
#include <cstring>

#include "check.h"

using namespace aow;

int main() {
    // Defaults are the firmware's.
    const Settings d;
    CHECK(d.gains.kp == 40 && d.gains.minPwm == 10 && d.gains.highPwm == 255);
    CHECK(d.gains.steerSensorCounts == 30 && d.steer.minSpeed == 3 && d.steer.softStartDurationMs == 500);
    CHECK(d.can.moduleId == 0x1C && d.misc.ledBrightness == 25);
    CHECK(d.log.consoleLevel == log::Severity::Info && d.log.udpLevel == log::Severity::Warning);
    CHECK(!d.isobus.enabled && d.isobus.channel == 2 && d.isobus.tractorEcu && d.isobus.terminal);

    // Round trip of changed values.
    Settings s;
    s.steer.invertWas = true;
    s.steer.currentThreshold = 77;
    s.gains.kp = 55.5f;
    s.gains.wasOffset = -123;
    s.gains.ackermanFix = 0.8f;
    s.ports.rate[0] = 38400;
    s.can.brand = 4;
    s.can.bus[2].function = 16;
    s.can.bus[2].speed = 1;
    s.misc.buzzerVolume = 2;
    s.isobus.enabled = true;
    s.isobus.channel = 3;
    s.isobus.tractorEcu = false;
    s.isobus.terminal = false;
    s.log.udpEnabled = true;
    s.log.consoleLevel = log::Severity::Debug;
    Settings r;
    CHECK(fromJson(toJson(s), r));
    CHECK(r.steer.invertWas && r.steer.currentThreshold == 77);
    CHECK(r.gains.kp == 55.5f && r.gains.wasOffset == -123 && r.gains.ackermanFix == 0.8f);
    CHECK(r.ports.rate[0] == 38400 && r.can.brand == 4);
    CHECK(r.can.bus[2].function == 16 && r.can.bus[2].speed == 1 && r.can.bus[0].function == 0);
    CHECK(r.misc.buzzerVolume == 2 && r.log.udpEnabled && r.log.consoleLevel == log::Severity::Debug);
    CHECK(r.isobus.enabled && r.isobus.channel == 3 && !r.isobus.tractorEcu && !r.isobus.terminal);
    CHECK(toJson(r) == toJson(s));

    // An ISOBUS channel that does not exist falls back to CAN2.
    Settings iso;
    CHECK(fromJson(R"({"isobus":{"enabled":true,"channel":7}})", iso));
    CHECK(iso.isobus.enabled && iso.isobus.channel == 2);

    // Partial file: only the keys present change; the rest keep their current values.
    Settings p = s;
    CHECK(fromJson(R"({"gains":{"minPwm":20},"log":{"udpLevel":"error"}})", p));
    CHECK(p.gains.minPwm == 20 && p.gains.kp == 55.5f && p.steer.invertWas);
    CHECK(p.log.udpLevel == log::Severity::Error);

    // Wrong types and unknown keys are ignored; out-of-range values are clamped.
    CHECK(fromJson(R"({"gains":{"kp":"fast"},"nonsense":1,"misc":{"ledBrightness":1,"buzzerVolume":9},
                      "steer":{"softStartDurationMs":5000}})", p));
    CHECK(p.gains.kp == 55.5f && p.misc.ledBrightness == 5 && p.misc.buzzerVolume == 1);
    CHECK(p.steer.softStartDurationMs == 1000);

    // Ports: nothing set by default (detection does the rest); round trip; bad values ignored.
    using P = Settings::Ports;
    CHECK(d.ports.nmeaOut == P::kNone && d.ports.rtcmOut == P::kNone && d.ports.keya == P::kNone);
    CHECK(d.ports.rate[0] == 0 && d.ports.rate[1] == 0 && d.ports.nmeaRateHz == 0 && !d.ports.nmeaOutUsb[0]);
    CHECK(d.ports.nmeaCorrected);
    Settings ps;
    ps.ports.nmeaOut = P::kUsb;
    std::snprintf(ps.ports.nmeaOutUsb, P::kUsbName, "usb-FTDI_FT232R_USB_UART_A1B2C3-if00-port0");
    ps.ports.rtcmOut = P::kPort2;
    ps.ports.rate[1] = 38400;
    ps.ports.nmeaSentences = P::kGga | P::kRmc;
    ps.ports.nmeaRateHz = 5;
    ps.ports.nmeaCorrected = false;
    ps.ports.keya = P::kPort1;
    Settings pr;
    CHECK(fromJson(toJson(ps), pr));
    CHECK(toJson(pr) == toJson(ps) && !std::strcmp(pr.ports.nmeaOutUsb, ps.ports.nmeaOutUsb));
    CHECK(fromJson(R"({"ports":{"nmeaOut":"moon","keya":"usb","rate":[14400,0],"nmeaRateHz":3}})", pr));
    CHECK(pr.ports.nmeaOut == P::kUsb && pr.ports.keya == P::kPort1);  // not changed
    CHECK(pr.ports.rate[0] == 0 && pr.ports.rate[1] == 0 && pr.ports.nmeaRateHz == 0);
    Settings old;  // a file from when the ports were set by hand: left to detection
    CHECK(fromJson(R"({"gps":{"radioBaud":57600},"ports":{"gnss":"port1","radio":"port2","baud":[9600,9600]}})", old));
    CHECK(old.ports.rate[0] == 0 && old.ports.nmeaOut == P::kNone);

    // What the page may save.
    CHECK(portsError(d.ports).empty() && portsError(ps.ports).empty());
    P bad;
    bad.nmeaOut = P::kUsb;  // no device chosen
    CHECK(!portsError(bad).empty());
    bad = P();
    bad.rate[0] = 14400;
    CHECK(!portsError(bad).empty());
    bad = P();
    bad.keya = P::kPort2;
    bad.rtcmOut = P::kPort2;
    CHECK(!portsError(bad).empty());
    bad = P();
    bad.nmeaOut = P::kPort1;
    bad.rtcmOut = P::kPort1;  // both outputs on one port: fine
    CHECK(portsError(bad).empty());

    // Broken JSON: rejected, settings untouched, reason given.
    Settings before = p;
    std::string err;
    CHECK(!fromJson("{\"gains\": {", p, &err) && !err.empty());
    CHECK(!fromJson("[1,2]", p, &err));
    CHECK(toJson(p) == toJson(before));

    return finish("settings");
}
