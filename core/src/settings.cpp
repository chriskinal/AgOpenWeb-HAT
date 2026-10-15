#include "aow/settings.h"

#include <ArduinoJson.h>

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
    for (int d = 0; d < P::kDevices; ++d)
        ports[P::deviceName(static_cast<P::Device>(d))] = P::whereName(static_cast<P::Where>(s.ports.where[d]));
    JsonArray baud = ports["baud"].to<JsonArray>();
    for (uint32_t b : s.ports.baud) baud.add(b);
    ports["nmeaSentences"] = s.ports.nmeaSentences;
    ports["nmeaRateHz"] = s.ports.nmeaRateHz;

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
    uint32_t oldRadioBaud = 0;  // before the ports settings: RS-232 #1's
    get(gps, "radioBaud", oldRadioBaud);

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

    using P = Settings::Ports;
    JsonVariantConst ports = doc["ports"];
    for (int d = 0; d < P::kDevices; ++d) {
        const char* w = ports[P::deviceName(static_cast<P::Device>(d))] | "";
        for (int i = P::kNone; i <= P::kOnboard; ++i)
            if (!std::strcmp(w, P::whereName(static_cast<P::Where>(i))) && P::allowed(static_cast<P::Device>(d), static_cast<P::Where>(i)))
                n.ports.where[d] = static_cast<uint8_t>(i);
    }
    JsonArrayConst baud = ports["baud"];
    for (size_t i = 0; i < 2 && i < baud.size(); ++i)
        if (baud[i].is<uint32_t>() && validBaud(baud[i].as<uint32_t>())) n.ports.baud[i] = baud[i];
    if (baud.isNull() && validBaud(oldRadioBaud)) n.ports.baud[0] = oldRadioBaud;
    get(ports, "nmeaSentences", n.ports.nmeaSentences);
    get(ports, "nmeaRateHz", n.ports.nmeaRateHz);
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

bool Settings::Ports::allowed(Device d, Where w) {
    switch (w) {
    case kNone: case kPort1: case kPort2: return true;
    case kUsb: return d == kGnss || d == kImu;  // the USB devices it can find: TM171, a receiver
    case kOnboard: return d == kGnss;            // the receiver on J7
    }
    return false;
}

const char* Settings::Ports::deviceName(Device d) {
    static const char* const kNames[kDevices] = {"gnss", "radio", "imu", "keya", "nmeaOut", "rtcmOut"};
    return d < kDevices ? kNames[d] : "";
}

const char* Settings::Ports::whereName(Where w) {
    static const char* const kNames[] = {"none", "port1", "port2", "usb", "onboard"};
    return w <= kOnboard ? kNames[w] : "none";
}

bool validBaud(uint32_t baud) {
    static const uint32_t kBauds[] = {4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
    for (uint32_t b : kBauds)
        if (b == baud) return true;
    return false;
}

PortPlan planPorts(const Settings::Ports& p, const std::string rs232[2], const std::string& onboardGnss,
                   unsigned onboardBaud) {
    using P = Settings::Ports;
    PortPlan plan;
    bool taken[2] = {false, false};
    for (int d = 0; d < P::kDevices; ++d) {
        const auto dev = static_cast<P::Device>(d);
        const auto w = static_cast<P::Where>(p.where[d]);
        if (!P::allowed(dev, w)) continue;
        PortPlan::Dev& out = plan.dev[d];
        if (w == P::kPort1 || w == P::kPort2) {
            const int i = w == P::kPort1 ? 0 : 1;
            if (rs232[i].empty() || taken[i]) continue;
            taken[i] = true;
            out.path = rs232[i];
            out.port = i;
            out.baud = dev == P::kImu || dev == P::kKeya ? 115200 : p.baud[i];
        } else if (w == P::kUsb) {
            out.usb = true;
            out.baud = dev == P::kImu ? 115200 : 460800;  // ignored by USB serial (CDC) devices
        } else if (w == P::kOnboard && !onboardGnss.empty()) {
            out.path = onboardGnss;
            out.baud = onboardBaud;
        }
    }
    return plan;
}

std::string portsError(const Settings::Ports& p) {
    using P = Settings::Ports;
    int on[2] = {0, 0};
    for (int d = 0; d < P::kDevices; ++d) {
        if (!P::allowed(static_cast<P::Device>(d), static_cast<P::Where>(p.where[d])))
            return std::string("That place does not suit the ") + P::deviceName(static_cast<P::Device>(d)) + ".";
        if (p.where[d] == P::kPort1) ++on[0];
        if (p.where[d] == P::kPort2) ++on[1];
    }
    for (int i = 0; i < 2; ++i)
        if (on[i] > 1) return "RS-232 port " + std::to_string(i + 1) + " has more than one device.";
    for (uint32_t b : p.baud)
        if (!validBaud(b)) return "Unsupported baud rate.";
    return {};
}

}  // namespace aow
