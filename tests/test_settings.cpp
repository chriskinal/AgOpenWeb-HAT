#include "aow/settings.h"

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
    s.ports.baud[0] = 38400;
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
    CHECK(r.ports.baud[0] == 38400 && r.can.brand == 4);
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

    // Ports: the firmware's layout by default; round trip; bad places and rates ignored.
    using P = Settings::Ports;
    CHECK(d.ports.where[P::kGnss] == P::kOnboard && d.ports.where[P::kRadio] == P::kPort1);
    CHECK(d.ports.where[P::kKeya] == P::kPort2 && d.ports.where[P::kImu] == P::kNone);
    CHECK(d.ports.where[P::kNmeaOut] == P::kNone && d.ports.where[P::kRtcmOut] == P::kNone);
    CHECK(d.ports.baud[0] == 115200 && d.ports.baud[1] == 115200 && d.ports.nmeaRateHz == 0);
    Settings ps;
    ps.ports.where[P::kImu] = P::kUsb;
    ps.ports.where[P::kRadio] = P::kNone;
    ps.ports.where[P::kNmeaOut] = P::kPort1;
    ps.ports.baud[0] = 38400;
    ps.ports.nmeaSentences = P::kGga | P::kRmc;
    ps.ports.nmeaRateHz = 5;
    Settings pr;
    CHECK(fromJson(toJson(ps), pr));
    CHECK(toJson(pr) == toJson(ps));
    CHECK(fromJson(R"({"ports":{"keya":"usb","radio":"moon","imu":"port2","baud":[14400,9600],"nmeaRateHz":3}})", pr));
    CHECK(pr.ports.where[P::kKeya] == P::kPort2 && pr.ports.where[P::kRadio] == P::kNone);  // not changed
    CHECK(pr.ports.where[P::kImu] == P::kPort2 && pr.ports.baud[0] == 38400 && pr.ports.baud[1] == 9600);
    CHECK(pr.ports.nmeaRateHz == 0);
    Settings old;  // a file from before the ports settings: its radio baud is RS-232 #1's
    CHECK(fromJson(R"({"gps":{"radioBaud":57600}})", old) && old.ports.baud[0] == 57600);

    // Which device does what.
    const std::string dev[2] = {"/dev/ttyAMA2", "/dev/ttyAMA3"};
    PortPlan plan = planPorts(d.ports, dev, "/dev/ttyAMA5", 460800);
    CHECK(plan.dev[P::kGnss].path == "/dev/ttyAMA5" && plan.dev[P::kGnss].baud == 460800);
    CHECK(plan.dev[P::kRadio].path == dev[0] && plan.dev[P::kRadio].port == 0 && plan.dev[P::kRadio].baud == 115200);
    CHECK(plan.dev[P::kKeya].path == dev[1] && plan.dev[P::kImu].path.empty() && !plan.dev[P::kImu].usb);
    plan = planPorts(ps.ports, dev, "/dev/ttyAMA5", 460800);
    CHECK(plan.dev[P::kImu].usb && plan.dev[P::kImu].path.empty() && plan.dev[P::kRadio].path.empty());
    CHECK(plan.dev[P::kNmeaOut].path == dev[0] && plan.dev[P::kNmeaOut].baud == 38400);
    P u;
    u.where[P::kGnss] = P::kPort1;   // an external receiver on #1, at #1's rate
    u.where[P::kRadio] = P::kNone;
    u.where[P::kImu] = P::kPort2;    // an IMU on #2 runs at 115200 whatever #2's rate
    u.where[P::kKeya] = P::kNone;
    u.baud[0] = 230400;
    u.baud[1] = 9600;
    plan = planPorts(u, dev, "/dev/ttyAMA5", 460800);
    CHECK(plan.dev[P::kGnss].path == dev[0] && plan.dev[P::kGnss].baud == 230400);
    CHECK(plan.dev[P::kImu].path == dev[1] && plan.dev[P::kImu].baud == 115200);
    CHECK(portsError(u).empty());
    u.where[P::kRtcmOut] = P::kPort2;  // two on #2 (a hand-edited file): the first has it
    plan = planPorts(u, dev, "/dev/ttyAMA5", 460800);
    CHECK(plan.dev[P::kImu].path == dev[1] && plan.dev[P::kRtcmOut].path.empty());
    CHECK(!portsError(u).empty());
    const std::string none[2] = {"", ""};  // not on the HAT: no RS-232 ports, no onboard receiver
    plan = planPorts(d.ports, none, "", 460800);
    CHECK(plan.dev[P::kRadio].path.empty() && plan.dev[P::kKeya].path.empty() && plan.dev[P::kGnss].path.empty());

    // What the page may save.
    CHECK(portsError(d.ports).empty() && portsError(ps.ports).empty());
    P bad;
    bad.where[P::kKeya] = P::kUsb;
    CHECK(!portsError(bad).empty());
    bad = P();
    bad.where[P::kRadio] = P::kOnboard;
    CHECK(!portsError(bad).empty());
    bad = P();
    bad.baud[1] = 14400;
    CHECK(!portsError(bad).empty());
    CHECK(P::allowed(P::kGnss, P::kUsb) && P::allowed(P::kImu, P::kUsb) && !P::allowed(P::kNmeaOut, P::kUsb));

    // Broken JSON: rejected, settings untouched, reason given.
    Settings before = p;
    std::string err;
    CHECK(!fromJson("{\"gains\": {", p, &err) && !err.empty());
    CHECK(!fromJson("[1,2]", p, &err));
    CHECK(toJson(p) == toJson(before));

    return finish("settings");
}
