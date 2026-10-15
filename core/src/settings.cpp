#include "aow/settings.h"

#include <ArduinoJson.h>

namespace aow {

namespace {

constexpr int kVersion = 1;

// Reads obj[key] into v if present and of a compatible type.
template <typename T>
void get(JsonVariantConst obj, const char* key, T& v) {
    JsonVariantConst x = obj[key];
    if (x.is<T>()) v = x.as<T>();
}

void getSeverity(JsonVariantConst obj, const char* key, log::Severity& v) {
    JsonVariantConst x = obj[key];
    log::Severity s;
    if (x.is<const char*>() && log::parseSeverity(x.as<const char*>(), s)) v = s;
}

}  // namespace

std::string toJson(const Settings& s) {
    JsonDocument doc;
    doc["version"] = kVersion;

    JsonObject st = doc["steer"].to<JsonObject>();
    st["invertWas"] = s.steer.invertWas;
    st["motorDriveDirection"] = s.steer.motorDriveDirection;
    st["cytronDriver"] = s.steer.cytronDriver;
    st["steerSwitch"] = s.steer.steerSwitch;
    st["steerButton"] = s.steer.steerButton;
    st["currentSensor"] = s.steer.currentSensor;
    st["useYAxis"] = s.steer.useYAxis;
    st["minSpeed"] = s.steer.minSpeed;
    st["motorDriverConfig"] = s.steer.motorDriverConfig;
    st["currentThreshold"] = s.steer.currentThreshold;
    st["pwmBrakeMode"] = s.steer.pwmBrakeMode;
    st["softStartDurationMs"] = s.steer.softStartDurationMs;

    JsonObject g = doc["gains"].to<JsonObject>();
    g["kp"] = s.gains.kp;
    g["highPwm"] = s.gains.highPwm;
    g["minPwm"] = s.gains.minPwm;
    g["steerSensorCounts"] = s.gains.steerSensorCounts;
    g["wasOffset"] = s.gains.wasOffset;
    g["ackermanFix"] = s.gains.ackermanFix;

    JsonObject gps = doc["gps"].to<JsonObject>();
    gps["passThrough"] = s.gps.passThrough;
    gps["radioBaud"] = s.gps.radioBaud;

    JsonObject can = doc["can"].to<JsonObject>();
    can["brand"] = s.can.brand;
    can["moduleId"] = s.can.moduleId;
    JsonArray buses = can["bus"].to<JsonArray>();
    for (const auto& b : s.can.bus) {
        JsonObject o = buses.add<JsonObject>();
        o["speed"] = b.speed;
        o["function"] = b.function;
        o["name"] = b.name;
    }

    JsonObject iso = doc["isobus"].to<JsonObject>();
    iso["enabled"] = s.isobus.enabled;
    iso["channel"] = s.isobus.channel;
    iso["tractorEcu"] = s.isobus.tractorEcu;
    iso["terminal"] = s.isobus.terminal;

    JsonObject ports = doc["ports"].to<JsonObject>();
    ports["rs232_1"] = s.ports.rs232[0];
    ports["rs232_2"] = s.ports.rs232[1];
    ports["usbImu"] = s.ports.usbImu;

    JsonObject m = doc["misc"].to<JsonObject>();
    m["invertWorkSwitch"] = s.misc.invertWorkSwitch;
    m["ledBrightness"] = s.misc.ledBrightness;
    m["buzzerVolume"] = s.misc.buzzerVolume;

    JsonObject l = doc["log"].to<JsonObject>();
    l["consoleEnabled"] = s.log.consoleEnabled;
    l["consoleLevel"] = log::levelName(s.log.consoleLevel);
    l["udpEnabled"] = s.log.udpEnabled;
    l["udpLevel"] = log::levelName(s.log.udpLevel);
    l["syslogPort"] = s.log.syslogPort;
    l["rateLimitDisabled"] = s.log.rateLimitDisabled;

    std::string out;
    serializeJsonPretty(doc, out);
    out += '\n';
    return out;
}

bool fromJson(const std::string& json, Settings& s, std::string* error) {
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, json);
    if (err || !doc.is<JsonObject>()) {
        if (error) *error = err ? err.c_str() : "not a JSON object";
        return false;
    }
    Settings n = s;

    JsonVariantConst st = doc["steer"];
    get(st, "invertWas", n.steer.invertWas);
    get(st, "motorDriveDirection", n.steer.motorDriveDirection);
    get(st, "cytronDriver", n.steer.cytronDriver);
    get(st, "steerSwitch", n.steer.steerSwitch);
    get(st, "steerButton", n.steer.steerButton);
    get(st, "currentSensor", n.steer.currentSensor);
    get(st, "useYAxis", n.steer.useYAxis);
    get(st, "minSpeed", n.steer.minSpeed);
    get(st, "motorDriverConfig", n.steer.motorDriverConfig);
    get(st, "currentThreshold", n.steer.currentThreshold);
    get(st, "pwmBrakeMode", n.steer.pwmBrakeMode);
    get(st, "softStartDurationMs", n.steer.softStartDurationMs);
    if (n.steer.softStartDurationMs > 1000) n.steer.softStartDurationMs = 1000;

    JsonVariantConst g = doc["gains"];
    get(g, "kp", n.gains.kp);
    get(g, "highPwm", n.gains.highPwm);
    get(g, "minPwm", n.gains.minPwm);
    get(g, "steerSensorCounts", n.gains.steerSensorCounts);
    get(g, "wasOffset", n.gains.wasOffset);
    get(g, "ackermanFix", n.gains.ackermanFix);

    JsonVariantConst gps = doc["gps"];
    get(gps, "passThrough", n.gps.passThrough);
    get(gps, "radioBaud", n.gps.radioBaud);

    JsonVariantConst can = doc["can"];
    get(can, "brand", n.can.brand);
    get(can, "moduleId", n.can.moduleId);
    JsonArrayConst buses = can["bus"];
    for (size_t i = 0; i < 3 && i < buses.size(); ++i) {
        get(buses[i], "speed", n.can.bus[i].speed);
        get(buses[i], "function", n.can.bus[i].function);
        get(buses[i], "name", n.can.bus[i].name);
    }

    JsonVariantConst iso = doc["isobus"];
    get(iso, "enabled", n.isobus.enabled);
    get(iso, "channel", n.isobus.channel);
    get(iso, "tractorEcu", n.isobus.tractorEcu);
    get(iso, "terminal", n.isobus.terminal);
    if (n.isobus.channel < 1 || n.isobus.channel > 3) n.isobus.channel = 2;

    JsonVariantConst ports = doc["ports"];
    get(ports, "rs232_1", n.ports.rs232[0]);
    get(ports, "rs232_2", n.ports.rs232[1]);
    get(ports, "usbImu", n.ports.usbImu);
    for (uint8_t& u : n.ports.rs232)
        if (u > Settings::Ports::kKeya) u = Settings::Ports::kOff;

    JsonVariantConst m = doc["misc"];
    get(m, "invertWorkSwitch", n.misc.invertWorkSwitch);
    get(m, "ledBrightness", n.misc.ledBrightness);
    get(m, "buzzerVolume", n.misc.buzzerVolume);
    if (n.misc.ledBrightness < 5) n.misc.ledBrightness = 5;
    if (n.misc.ledBrightness > 100) n.misc.ledBrightness = 100;
    if (n.misc.buzzerVolume > 2) n.misc.buzzerVolume = 1;

    JsonVariantConst l = doc["log"];
    get(l, "consoleEnabled", n.log.consoleEnabled);
    getSeverity(l, "consoleLevel", n.log.consoleLevel);
    get(l, "udpEnabled", n.log.udpEnabled);
    getSeverity(l, "udpLevel", n.log.udpLevel);
    get(l, "syslogPort", n.log.syslogPort);
    get(l, "rateLimitDisabled", n.log.rateLimitDisabled);

    s = n;
    return true;
}

PortPlan planPorts(const Settings::Ports& p, const std::string rs232[2]) {
    PortPlan plan;
    for (int i = 0; i < 2; ++i) {
        if (rs232[i].empty()) continue;
        std::string* to = p.rs232[i] == Settings::Ports::kRadio ? &plan.radio
                        : p.rs232[i] == Settings::Ports::kImu   ? &plan.imu
                        : p.rs232[i] == Settings::Ports::kKeya  ? &plan.keya
                                                                : nullptr;
        if (to && to->empty()) *to = rs232[i];
    }
    plan.usbImu = p.usbImu && plan.imu.empty();
    return plan;
}

std::string portsError(const Settings::Ports& p) {
    for (uint8_t u : p.rs232)
        if (u > Settings::Ports::kKeya) return "Unknown port use.";
    if (p.rs232[0] != Settings::Ports::kOff && p.rs232[0] == p.rs232[1])
        return "Both RS-232 ports are set to the same use.";
    if (p.usbImu && (p.rs232[0] == Settings::Ports::kImu || p.rs232[1] == Settings::Ports::kImu))
        return "Only one IMU: turn off the USB IMU or set the RS-232 port to something else.";
    return {};
}

}  // namespace aow
