#include "aow/settings.h"

#include "check.h"

using namespace aow;

int main() {
    // Defaults are the firmware's.
    const Settings d;
    CHECK(d.gains.kp == 40 && d.gains.minPwm == 10 && d.gains.highPwm == 255);
    CHECK(d.gains.steerSensorCounts == 30 && d.steer.minSpeed == 3 && d.steer.softStartDurationMs == 500);
    CHECK(d.gps.radioBaud == 115200 && d.can.moduleId == 0x1C && d.misc.ledBrightness == 25);
    CHECK(d.log.consoleLevel == log::Severity::Info && d.log.udpLevel == log::Severity::Warning);

    // Round trip of changed values.
    Settings s;
    s.steer.invertWas = true;
    s.steer.currentThreshold = 77;
    s.gains.kp = 55.5f;
    s.gains.wasOffset = -123;
    s.gains.ackermanFix = 0.8f;
    s.gps.radioBaud = 38400;
    s.can.brand = 4;
    s.can.bus[2].function = 16;
    s.can.bus[2].speed = 1;
    s.misc.buzzerVolume = 2;
    s.log.udpEnabled = true;
    s.log.consoleLevel = log::Severity::Debug;
    Settings r;
    CHECK(fromJson(toJson(s), r));
    CHECK(r.steer.invertWas && r.steer.currentThreshold == 77);
    CHECK(r.gains.kp == 55.5f && r.gains.wasOffset == -123 && r.gains.ackermanFix == 0.8f);
    CHECK(r.gps.radioBaud == 38400 && r.can.brand == 4);
    CHECK(r.can.bus[2].function == 16 && r.can.bus[2].speed == 1 && r.can.bus[0].function == 0);
    CHECK(r.misc.buzzerVolume == 2 && r.log.udpEnabled && r.log.consoleLevel == log::Severity::Debug);
    CHECK(toJson(r) == toJson(s));

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

    // Broken JSON: rejected, settings untouched, reason given.
    Settings before = p;
    std::string err;
    CHECK(!fromJson("{\"gains\": {", p, &err) && !err.empty());
    CHECK(!fromJson("[1,2]", p, &err));
    CHECK(toJson(p) == toJson(before));

    return finish("settings");
}
