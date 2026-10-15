#include "aow/settings.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>

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

    using P = Settings::Ports;
    JsonObject ports = doc["ports"].to<JsonObject>();
    ports["nmeaOut"] = P::outName(s.ports.nmeaOut);
    ports["nmeaOutUsb"] = s.ports.nmeaOutUsb;
    ports["rtcmOut"] = P::outName(s.ports.rtcmOut);
    ports["rtcmOutUsb"] = s.ports.rtcmOutUsb;
    JsonArray rate = ports["rate"].to<JsonArray>();
    for (uint32_t r : s.ports.rate) rate.add(r);
    ports["nmeaSentences"] = s.ports.nmeaSentences;
    ports["nmeaRateHz"] = s.ports.nmeaRateHz;
    ports["nmeaCorrected"] = s.ports.nmeaCorrected;
    ports["keya"] = P::outName(s.ports.keya);

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

    // Older files' gnss, radio, imu and baud keys (when the ports were set by hand) are left
    // to detection: auto rates.
    using P = Settings::Ports;
    JsonVariantConst ports = doc["ports"];
    P::parseOut(ports["nmeaOut"] | "", n.ports.nmeaOut);
    P::parseOut(ports["rtcmOut"] | "", n.ports.rtcmOut);
    uint8_t keya = n.ports.keya;
    if (P::parseOut(ports["keya"] | "", keya) && keya != P::kUsb) n.ports.keya = keya;
    auto name = [&](const char* key, char* out) {
        JsonVariantConst v = ports[key];
        if (v.is<const char*>()) std::snprintf(out, P::kUsbName, "%s", v.as<const char*>());
    };
    name("nmeaOutUsb", n.ports.nmeaOutUsb);
    name("rtcmOutUsb", n.ports.rtcmOutUsb);
    JsonArrayConst rate = ports["rate"];
    for (size_t i = 0; i < 2 && i < rate.size(); ++i)
        if (rate[i].is<uint32_t>() && (rate[i].as<uint32_t>() == 0 || validBaud(rate[i].as<uint32_t>()))) n.ports.rate[i] = rate[i];
    get(ports, "nmeaSentences", n.ports.nmeaSentences);
    get(ports, "nmeaRateHz", n.ports.nmeaRateHz);
    get(ports, "nmeaCorrected", n.ports.nmeaCorrected);
    if (n.ports.nmeaRateHz != 1 && n.ports.nmeaRateHz != 2 && n.ports.nmeaRateHz != 5 && n.ports.nmeaRateHz != 10)
        n.ports.nmeaRateHz = 0;

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

const char* Settings::Ports::outName(uint8_t o) {
    static const char* const kNames[] = {"none", "port1", "port2", "usb"};
    return o <= kUsb ? kNames[o] : "none";
}

bool Settings::Ports::parseOut(const char* name, uint8_t& o) {
    for (uint8_t i = kNone; i <= kUsb; ++i)
        if (!std::strcmp(name, outName(i))) {
            o = i;
            return true;
        }
    return false;
}

bool validBaud(uint32_t baud) {
    static const uint32_t kBauds[] = {4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
    for (uint32_t b : kBauds)
        if (b == baud) return true;
    return false;
}

std::string portsError(const Settings::Ports& p) {
    using P = Settings::Ports;
    if (p.nmeaOut > P::kUsb || p.rtcmOut > P::kUsb) return "Unknown output place.";
    if (p.nmeaOut == P::kUsb && !p.nmeaOutUsb[0]) return "Choose the USB device for NMEA out.";
    if (p.rtcmOut == P::kUsb && !p.rtcmOutUsb[0]) return "Choose the USB device for RTCM out.";
    for (uint32_t r : p.rate)
        if (r != 0 && !validBaud(r)) return "Unsupported baud rate.";
    if (p.keya != P::kNone && (p.keya == p.nmeaOut || p.keya == p.rtcmOut))
        return "That port has the Keya motor (set in the settings file).";
    return {};
}

}  // namespace aow
