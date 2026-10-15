#include "api.h"

#define PROGMEM  // the firmware's pages are Arduino flash strings; plain const here

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>

#include "pages/CANConfigUploadPage.h"
#include "pages/CANInfoJSON.h"
#include "pages/DragDropCANConfigPage.h"
#include "pages/IsobusPage.h"
#include "pages/SerialPage.h"
#include "pages/TouchFriendlyDeviceSettingsPage.h"
#include "pages/TouchFriendlyEventLoggerPage.h"
#include "pages/TouchFriendlyGPSConfigPage.h"
#include "pages/TouchFriendlyHomePage.h"
#include "pages/TouchFriendlyLogViewerPage.h"
#include "pages/TouchFriendlyNetworkPage.h"
#include "pages/TouchFriendlyOTAPage.h"
#include "pages/TouchFriendlyStyles.h"

namespace aow::web {

namespace {

std::string toString(const JsonDocument& doc) {
    std::string out;
    serializeJson(doc, out);
    return out;
}

Response invalidJson() { return Response::json(R"({"status":"error","message":"Invalid JSON"})", 400); }
Response methodNotAllowed() { return Response::text("Method Not Allowed", 405); }

Response saveOr500(Backend& b, const Settings& s, const char* okBody) {
    std::string err;
    if (b.applySettings(s, err)) return Response::json(okBody);
    JsonDocument doc;
    doc["status"] = "error";
    doc["message"] = err;
    return Response::json(toString(doc), 500);
}

// A page: the firmware's PROGMEM strings, compiled in.
void page(Server& server, const char* path, const char* html) {
    server.route(path, [html](const Request&) { return Response::html(html); });
}

// The ISOBUS state, for /api/status and the ISOBUS page.
void isobusStatusJson(const IsobusStatus& is, JsonObject iso) {
    iso["state"] = isobusStateName(is.state);
    iso["channel"] = is.channel;
    iso["interface"] = std::string(is.interface);  // copied: the document outlives is
    iso["busActive"] = is.busActive;
    if (is.state == IsobusStatus::State::Claimed) iso["address"] = is.address;
    char name[17];
    std::snprintf(name, sizeof name, "%016llX", static_cast<unsigned long long>(is.name));
    iso["name"] = name;
    JsonObject tc = iso["taskController"].to<JsonObject>();
    tc["clients"] = is.clients;
    tc["implement"] = std::string(is.implement);
    tc["sections"] = is.sections;
    tc["width"] = is.widthM;
    tc["sectionControl"] = is.sectionControl;
    std::string setpoint, actual;  // one character a section, section 1 first
    for (uint8_t i = 0; i < is.sections && i < 64; ++i) {
        setpoint += (is.setpoint >> i) & 1 ? '1' : '0';
        actual += (is.actual >> i) & 1 ? '1' : '0';
    }
    tc["setpoint"] = setpoint;
    tc["actual"] = actual;
    JsonObject tecu = iso["tractorEcu"].to<JsonObject>();
    tecu["state"] = isobusTecuName(is.tecu);
    tecu["speed"] = is.speedMmS * 0.0036;  // km/h
    iso["terminal"] = isobusTerminalName(is.terminal);
}

Response status(Backend& b, const char* version) {
    const Status st = b.status();
    JsonDocument doc;
    doc["version"] = version;
    doc["uptime"] = st.uptimeMs;  // the firmware sends millis()
    doc["freeMemory"] = 0;
    JsonObject net = doc["network"].to<JsonObject>();
    const auto ifs = b.interfaces();
    const Backend::Interface* first = nullptr;
    for (const auto& i : ifs)
        if (i.up && !i.ip.empty()) { first = &i; break; }
    net["ip"] = first ? first->ip : "";
    net["connected"] = first != nullptr;
    net["linkSpeed"] = first ? first->speedMbps : 0;
    JsonArray all = net["interfaces"].to<JsonArray>();
    for (const auto& i : ifs) {
        JsonObject o = all.add<JsonObject>();
        o["name"] = i.name;
        o["ip"] = i.ip;
        o["up"] = i.up;
    }
    doc["deviceType"] = "Steer";
    doc["moduleId"] = 126;
    doc["esp32Detected"] = false;
    doc["esp32Active"] = false;
    doc["systemHealthy"] = true;
    JsonObject gnss = doc["gnss"].to<JsonObject>();
    gnss["fixQuality"] = st.fixQuality;
    gnss["satellites"] = st.satellites;
    doc["appConnected"] = st.haveAppLink;
    JsonObject imu = doc["imu"].to<JsonObject>();  // AOW HAT: not in the firmware's status
    imu["type"] = st.imuType;
    imu["valid"] = st.imuValid;
    imu["port"] = st.imuPort;
    if (st.imuValid) {
        imu["heading"] = st.imuHeading;
        imu["roll"] = st.imuRoll;
        imu["pitch"] = st.imuPitch;
        imu["yawRate"] = st.imuYawRate;
    }
    JsonObject serial = doc["serial"].to<JsonObject>();  // AOW HAT: the Serial Communications page's port matrix
    serial["gnssPort"] = st.gnssPort;
    serial["radioBytes"] = st.radioBytes;
    serial["nmeaOutSent"] = st.nmeaOutSent;
    serial["rtcmOutBytes"] = st.rtcmOutBytes;
    JsonArray rs232 = serial["rs232"].to<JsonArray>();
    for (const auto& v : st.rs232) {
        JsonObject o = rs232.add<JsonObject>();
        o["use"] = v.use;
        o["open"] = v.open;
        o["bytesIn"] = v.bytesIn;
        if (v.msSinceIn != UINT32_MAX) o["msSinceIn"] = v.msSinceIn;
        JsonArray lines = o["lines"].to<JsonArray>();
        for (int l = 0; l < v.lineCount; ++l) lines.add(std::string(v.lines[l]));  // copied: the document outlives st
        JsonArray types = o["rtcm"].to<JsonArray>();
        for (int t = 0; t < v.rtcmCount; ++t) types.add(v.rtcm[t]);
    }
    JsonArray canRestarts = doc["canRestarts"].to<JsonArray>();  // AOW HAT: see daemon/can_watch.h
    for (uint32_t n : st.canRestarts) canRestarts.add(n);
    isobusStatusJson(b.isobusStatus(), doc["isobus"].to<JsonObject>());  // AOW HAT: not in the firmware's status
    static const char* const kColors[] = {"off", "red", "amber", "green", "blue"};
    JsonArray leds = doc["leds"].to<JsonArray>();
    for (uint8_t c : st.leds) leds.add(kColors[c < 5 ? c : 0]);
    return Response::json(toString(doc));
}

Response eventLogger(Backend& b, const Request& r) {
    Settings s = b.settings();
    if (r.method == "GET") {
        JsonDocument doc;
        doc["serialEnabled"] = s.log.consoleEnabled;
        doc["serialLevel"] = static_cast<int>(s.log.consoleLevel);
        doc["udpEnabled"] = s.log.udpEnabled;
        doc["udpLevel"] = static_cast<int>(s.log.udpLevel);
        doc["rateLimitDisabled"] = s.log.rateLimitDisabled;
        return Response::json(toString(doc));
    }
    if (r.method != "POST") return methodNotAllowed();
    JsonDocument doc;
    if (deserializeJson(doc, r.body) || !doc.is<JsonObject>()) return invalidJson();
    if (doc["serialEnabled"].is<bool>()) s.log.consoleEnabled = doc["serialEnabled"];
    if (doc["udpEnabled"].is<bool>()) s.log.udpEnabled = doc["udpEnabled"];
    if (doc["rateLimitDisabled"].is<bool>()) s.log.rateLimitDisabled = doc["rateLimitDisabled"];
    if (doc["serialLevel"].is<int>()) {
        const int v = doc["serialLevel"];
        if (v >= 0 && v <= 7) s.log.consoleLevel = static_cast<log::Severity>(v);
    }
    if (doc["udpLevel"].is<int>()) {
        const int v = doc["udpLevel"];
        if (v >= 0 && v <= 7) s.log.udpLevel = static_cast<log::Severity>(v);
    }
    return saveOr500(b, s, R"({"status":"saved"})");
}

void putLogs(JsonDocument& doc, const log::Entry* entries, size_t n) {
    JsonArray logs = doc["logs"].to<JsonArray>();
    for (size_t i = 0; i < n; ++i) {
        const log::Entry& e = entries[i];
        JsonObject o = logs.add<JsonObject>();
        o["timestamp"] = e.timestampMs;
        o["severity"] = static_cast<int>(e.severity);
        o["source"] = static_cast<int>(e.source);
        o["severityName"] = log::severityName(e.severity);
        o["sourceName"] = log::sourceName(e.source);
        o["message"] = e.message;
    }
}

Response logData() {
    static log::Entry entries[log::Logger::kHistory];
    const size_t n = log::global().history(entries, log::Logger::kHistory);
    JsonDocument doc;
    putLogs(doc, entries, n);
    return Response::json(toString(doc));
}

// The RS-232 port the radio is on (0 or 1); port 1 when it is on neither.
int radioPortIndex(const Settings::Ports& p) {
    return p.where[Settings::Ports::kRadio] == Settings::Ports::kPort2 ? 1 : 0;
}

Response deviceSettings(Backend& b, const Request& r) {
    Settings s = b.settings();
    if (r.method == "GET") {
        JsonDocument doc;
        doc["deviceType"] = "Steer";
        doc["moduleId"] = 126;
        doc["udpPassthrough"] = s.gps.passThrough;
        doc["pwmBrakeMode"] = s.steer.pwmBrakeMode;
        doc["softStartDuration"] = s.steer.softStartDurationMs;
        doc["serialRadioBaud"] = s.ports.baud[radioPortIndex(s.ports)];  // the radio's port's
        doc["buzzerVolume"] = s.misc.buzzerVolume;
        doc["invertWorkSwitch"] = s.misc.invertWorkSwitch;
        doc["ledBrightness"] = s.misc.ledBrightness;
        return Response::json(toString(doc));
    }
    if (r.method != "POST") return methodNotAllowed();
    JsonDocument doc;
    if (deserializeJson(doc, r.body) || !doc.is<JsonObject>()) return invalidJson();
    // Unlike the firmware, a field left out keeps its value rather than reverting to default.
    if (doc["udpPassthrough"].is<bool>()) s.gps.passThrough = doc["udpPassthrough"];
    if (doc["pwmBrakeMode"].is<bool>()) s.steer.pwmBrakeMode = doc["pwmBrakeMode"];
    if (doc["softStartDuration"].is<int>()) {
        const int v = doc["softStartDuration"];
        s.steer.softStartDurationMs = static_cast<uint16_t>(v < 0 ? 0 : v > 1000 ? 1000 : v);
    }
    if (doc["buzzerVolume"].is<int>()) {
        const int v = doc["buzzerVolume"];
        if (v >= 0 && v <= 2) s.misc.buzzerVolume = static_cast<uint8_t>(v);
    }
    if (doc["invertWorkSwitch"].is<bool>()) s.misc.invertWorkSwitch = doc["invertWorkSwitch"];
    if (doc["ledBrightness"].is<int>()) {
        const int v = doc["ledBrightness"];
        s.misc.ledBrightness = static_cast<uint8_t>(v < 5 ? 5 : v > 100 ? 100 : v);
    }
    // The firmware's radio rate, no longer on this page: the radio's port's (/api/serial).
    if (doc["serialRadioBaud"].is<uint32_t>() && validBaud(doc["serialRadioBaud"].as<uint32_t>()))
        s.ports.baud[radioPortIndex(s.ports)] = doc["serialRadioBaud"];
    return saveOr500(b, s, R"({"status":"saved"})");
}

// AOW HAT: the Serial Communications page: where each serial device is (Settings::Ports), in
// the settings file's form. Refused whole, with the reason, if it does not fit (two devices on
// a port). Baud rates apply live; a move or NMEA out's choices at the next start, so a save
// says whether one changed.
Response serial(Backend& b, const Request& r) {
    using P = Settings::Ports;
    Settings s = b.settings();
    if (r.method == "GET") {
        JsonDocument doc;
        for (int d = 0; d < P::kDevices; ++d)
            doc[P::deviceName(static_cast<P::Device>(d))] = P::whereName(static_cast<P::Where>(s.ports.where[d]));
        JsonArray baud = doc["baud"].to<JsonArray>();
        for (uint32_t v : s.ports.baud) baud.add(v);
        doc["nmeaSentences"] = s.ports.nmeaSentences;
        doc["nmeaRateHz"] = s.ports.nmeaRateHz;
        return Response::json(toString(doc));
    }
    if (r.method != "POST") return methodNotAllowed();
    JsonDocument doc;
    if (deserializeJson(doc, r.body) || !doc.is<JsonObject>()) return invalidJson();
    const P before = s.ports;
    for (int d = 0; d < P::kDevices; ++d) {
        JsonVariantConst w = doc[P::deviceName(static_cast<P::Device>(d))];
        if (!w.is<const char*>()) continue;
        bool known = false;
        for (int i = P::kNone; i <= P::kOnboard; ++i)
            if (!std::strcmp(w.as<const char*>(), P::whereName(static_cast<P::Where>(i)))) {
                s.ports.where[d] = static_cast<uint8_t>(i);
                known = true;
            }
        if (!known) s.ports.where[d] = 0xFF;  // portsError() names it
    }
    JsonArrayConst baud = doc["baud"];
    for (size_t i = 0; i < 2 && i < baud.size(); ++i)
        if (baud[i].is<uint32_t>()) s.ports.baud[i] = baud[i];
    if (doc["nmeaSentences"].is<int>()) s.ports.nmeaSentences = static_cast<uint8_t>(doc["nmeaSentences"].as<int>() & 0x1F);
    if (doc["nmeaRateHz"].is<int>()) {
        const int v = doc["nmeaRateHz"];
        if (v == 0 || v == 1 || v == 2 || v == 5 || v == 10) s.ports.nmeaRateHz = static_cast<uint8_t>(v);
    }
    const std::string err = portsError(s.ports);
    if (!err.empty()) {
        JsonDocument e;
        e["status"] = "error";
        e["message"] = err;
        return Response::json(toString(e), 400);
    }
    bool restart = before.nmeaSentences != s.ports.nmeaSentences || before.nmeaRateHz != s.ports.nmeaRateHz;
    for (int d = 0; d < P::kDevices; ++d) restart |= before.where[d] != s.ports.where[d];
    return saveOr500(b, s, restart ? R"({"status":"saved","restartRequired":true})"
                                   : R"({"status":"saved","restartRequired":false})");
}

// AOW HAT: the ISOBUS page's settings and live state. The settings apply at the next start of
// the daemon, so a save says whether anything changed.
Response isobus(Backend& b, const Request& r) {
    Settings s = b.settings();
    if (r.method == "GET") {
        JsonDocument doc;
        doc["enabled"] = s.isobus.enabled;
        doc["channel"] = s.isobus.channel;
        doc["tractorEcu"] = s.isobus.tractorEcu;
        doc["terminal"] = s.isobus.terminal;
        isobusStatusJson(b.isobusStatus(), doc["status"].to<JsonObject>());
        return Response::json(toString(doc));
    }
    if (r.method != "POST") return methodNotAllowed();
    JsonDocument doc;
    if (deserializeJson(doc, r.body) || !doc.is<JsonObject>()) return invalidJson();
    const Settings::Isobus before = s.isobus;
    if (doc["enabled"].is<bool>()) s.isobus.enabled = doc["enabled"];
    if (doc["tractorEcu"].is<bool>()) s.isobus.tractorEcu = doc["tractorEcu"];
    if (doc["terminal"].is<bool>()) s.isobus.terminal = doc["terminal"];
    if (doc["channel"].is<int>()) {
        const int v = doc["channel"];
        if (v >= 1 && v <= 3) s.isobus.channel = static_cast<uint8_t>(v);
    }
    const bool changed = before.enabled != s.isobus.enabled || before.channel != s.isobus.channel ||
                         before.tractorEcu != s.isobus.tractorEcu || before.terminal != s.isobus.terminal;
    return saveOr500(b, s, changed ? R"({"status":"saved","restartRequired":true})"
                                   : R"({"status":"saved","restartRequired":false})");
}

Response networkConfig(Backend& b, const Request& r) {
    if (r.method == "GET") {
        JsonDocument doc;
        JsonArray ip = doc["ip"].to<JsonArray>();
        for (const auto& i : b.interfaces()) {
            if (!i.up || i.ip.empty()) continue;
            unsigned a[4];
            if (std::sscanf(i.ip.c_str(), "%u.%u.%u.%u", &a[0], &a[1], &a[2], &a[3]) == 4)
                for (unsigned v : a) ip.add(v);
            break;
        }
        return Response::json(toString(doc));
    }
    if (r.method != "POST") return methodNotAllowed();
    return Response::json(R"({"status":"error","error":"On the HAT the CM4's network is set up in the operating system (Wi-Fi and Ethernet settings), not here. The HAT talks to AgOpenWeb on the same machine, so it needs no address of its own."})");
}

Response canConfig(Backend& b, const Request& r) {
    Settings s = b.settings();
    if (r.method == "GET") {
        JsonDocument doc;
        doc["brand"] = s.can.brand;
        for (int i = 0; i < 3; ++i) {
            const std::string n = "can" + std::to_string(i + 1);
            doc[n + "Speed"] = s.can.bus[i].speed;
            doc[n + "Function"] = s.can.bus[i].function;
            doc[n + "Name"] = s.can.bus[i].name;
        }
        doc["moduleID"] = s.can.moduleId;
        return Response::json(toString(doc));
    }
    if (r.method != "POST") return methodNotAllowed();
    JsonDocument doc;
    if (deserializeJson(doc, r.body) || !doc.is<JsonObject>()) return Response::json(R"({"status":"error","message":"Invalid JSON"})");
    auto take = [&](const std::string& key, uint8_t& v) {
        if (doc[key].is<uint8_t>()) v = doc[key].as<uint8_t>();
    };
    take("brand", s.can.brand);
    for (int i = 0; i < 3; ++i) {
        const std::string n = "can" + std::to_string(i + 1);
        take(n + "Speed", s.can.bus[i].speed);
        take(n + "Function", s.can.bus[i].function);
        take(n + "Name", s.can.bus[i].name);
    }
    take("moduleID", s.can.moduleId);
    for (int i = 0; i < 3; ++i)
        for (int j = i + 1; j < 3; ++j)
            if (s.can.bus[i].name != 0 && s.can.bus[i].name == s.can.bus[j].name)
                return Response::json(R"({"status":"error","message":"Each bus name can only be used once"})");
    std::string err;
    if (!b.applySettings(s, err)) return Response::json(R"({"status":"error","message":"Could not save"})", 500);
    LOG_INFO(Network, "CAN config saved - Brand: %u, CAN1: %u/%u, CAN2: %u/%u, CAN3: %u/%u", s.can.brand,
             s.can.bus[0].speed, s.can.bus[0].function, s.can.bus[1].speed, s.can.bus[1].function,
             s.can.bus[2].speed, s.can.bus[2].function);
    return Response::json(R"({"status":"ok","message":"Configuration saved. Restart required."})");
}

Response canInfo(Backend& b) {
    const std::string custom = b.customCanConfig();
    return Response::json(custom.empty() ? std::string(CAN_INFO_JSON) : custom);
}

Response canUpload(Backend& b, const Request& r) {
    if (r.method != "POST") return methodNotAllowed();
    if (r.body.empty()) return Response::text("No content received", 400);
    JsonDocument test;
    const DeserializationError e = deserializeJson(test, r.body, DeserializationOption::NestingLimit(20));
    if (e) return Response::text(std::string("Invalid JSON: ") + e.c_str(), 400);
    std::string err;
    if (!b.writeCustomCanConfig(r.body, err)) return Response::text("Failed to write: " + err, 500);
    LOG_INFO(Network, "Custom CAN config uploaded (%zu bytes)", r.body.size());
    return Response::text("Upload successful");
}

// System update. The daemon only takes delivery: steering must be off, and the file must look
// like a package. Verifying the signature and installing is the updater service's job.
Response otaUpload(Backend& b, const Request& r) {
    if (r.method != "POST") return methodNotAllowed();
    if (b.status().autosteerActive) return Response::text("Steering is engaged. Disengage it, then update.", 409);
    if (!looksLikeUpdatePackage(r.body)) return Response::text("This is not an AgOpenWeb HAT update file (.aowpkg).", 400);
    std::string err;
    if (!b.startUpdate(r.body, err)) return Response::text(err, 500);
    LOG_NOTICE(System, "Update package received (%zu bytes); handed to the updater", r.body.size());
    return Response::text("Package received");
}

Response otaStatus(Backend& b) {
    const std::string s = b.updateStatus();
    return Response::json(s.empty() ? R"({"state":"idle","message":""})" : s);
}

Response otaReboot(Backend& b, const Request& r) {
    if (r.method != "POST") return methodNotAllowed();
    if (b.status().autosteerActive) return Response::text("Steering is engaged. Disengage it first.", 409);
    b.reboot();
    return Response::json(R"({"status":"rebooting"})");
}

Response canRestore(Backend& b, const Request& r) {
    if (r.method != "POST") return methodNotAllowed();
    std::string err;
    if (!b.deleteCustomCanConfig(err)) return Response::text("Failed to restore default: " + err, 500);
    LOG_INFO(Network, "Custom CAN config deleted, restored to default");
    return Response::text("Default configuration restored");
}

Response canStatus(Backend& b) {
    const std::string custom = b.customCanConfig();
    JsonDocument doc;
    doc["custom"] = !custom.empty();
    if (custom.empty()) {
        doc["version"] = "2.0";
    } else {
        JsonDocument cfg;
        doc["version"] = !deserializeJson(cfg, custom) && !cfg["version"].isNull() ? cfg["version"].as<std::string>()
                                                                                   : std::string("Unknown");
        doc["size"] = custom.size();
    }
    return Response::json(toString(doc));
}

Response um98xRead(Backend& b) {
    Um98x::Config cfg;
    std::string err;
    JsonDocument doc;
    const bool ok = b.um98xRead(cfg, err);
    doc["success"] = ok;
    if (ok) {
        doc["config"] = cfg.configCommands;
        doc["mode"] = cfg.modeSettings;
        doc["messages"] = cfg.messageSettings;
    } else {
        doc["error"] = err.empty() ? "Failed to read GPS configuration" : err;
    }
    return Response::json(toString(doc), ok ? 200 : 500);
}

Response um98xWrite(Backend& b, const Request& r) {
    if (r.method != "POST") return methodNotAllowed();
    JsonDocument in;
    if (deserializeJson(in, r.body) || !in.is<JsonObject>())
        return Response::json(R"({"success":false,"error":"Invalid JSON"})", 400);
    Um98x::Config cfg;
    cfg.configCommands = in["config"] | "";
    cfg.modeSettings = in["mode"] | "";
    cfg.messageSettings = in["messages"] | "";
    std::string err;
    JsonDocument doc;
    const bool ok = b.um98xWrite(cfg, err);
    doc["success"] = ok;
    if (!ok) doc["error"] = err.empty() ? "Failed to write GPS configuration" : err;
    return Response::json(toString(doc), ok ? 200 : 500);
}

void put(std::string& out, size_t at, const void* v, size_t n) { std::memcpy(&out[at], v, n); }

}  // namespace

bool looksLikeUpdatePackage(const std::string& bytes) {
    static const char kTrailer[] = "AOWHAT1\n";
    const size_t trailer = sizeof kTrailer - 1, signature = 64;
    if (bytes.size() < 2 + signature + trailer + 64 || bytes.size() > kMaxUpdatePackage) return false;
    if (static_cast<uint8_t>(bytes[0]) != 0x1f || static_cast<uint8_t>(bytes[1]) != 0x8b) return false;  // gzip
    return bytes.compare(bytes.size() - trailer, trailer, kTrailer) == 0;
}

void addRoutes(Server& server, Backend& b, const char* version) {
    page(server, "/", TOUCH_FRIENDLY_HOME_PAGE);
    server.route("/touch.css", [](const Request&) {
        Response r;
        r.contentType = "text/css";
        r.body = TOUCH_FRIENDLY_CSS;
        return r;
    });
    page(server, "/device", TOUCH_FRIENDLY_DEVICE_SETTINGS_PAGE);
    page(server, "/eventlogger", TOUCH_FRIENDLY_EVENT_LOGGER_PAGE);
    page(server, "/logs", TOUCH_FRIENDLY_LOG_VIEWER_PAGE);
    page(server, "/network", TOUCH_FRIENDLY_NETWORK_PAGE);
    page(server, "/can", DRAG_DROP_CAN_CONFIG_PAGE);
    page(server, "/isobus", ISOBUS_PAGE);
    page(server, "/serial", SERIAL_PAGE);
    server.route("/api/isobus", [&b](const Request& r) { return isobus(b, r); });
    server.route("/api/serial", [&b](const Request& r) { return serial(b, r); });
    page(server, "/um98x-config", TOUCH_FRIENDLY_GPS_CONFIG_PAGE);
    page(server, "/gps", TOUCH_FRIENDLY_GPS_CONFIG_PAGE);
    server.route("/api/um98x/read", [&b](const Request&) { return um98xRead(b); });
    server.route("/api/um98x/write", [&b](const Request& r) { return um98xWrite(b, r); });
    page(server, "/can/upload", CAN_CONFIG_UPLOAD_PAGE);
    server.route("/api/can/config", [&b](const Request& r) { return canConfig(b, r); });
    server.route("/api/can/info", [&b](const Request&) { return canInfo(b); });
    server.route("/api/can/config/upload", [&b](const Request& r) { return canUpload(b, r); });
    server.route("/api/can/config/restore", [&b](const Request& r) { return canRestore(b, r); });
    server.route("/api/can/config/status", [&b](const Request&) { return canStatus(b); });

    server.route("/api/status", [&b, version](const Request&) { return status(b, version); });
    server.route("/api/restart", [&b](const Request& r) {
        if (r.method != "POST") return methodNotAllowed();
        b.restart();
        return Response::json(R"({"status":"restarting"})");
    });
    page(server, "/ota", TOUCH_FRIENDLY_OTA_PAGE);
    server.route("/api/ota/upload", [&b](const Request& r) { return otaUpload(b, r); });
    server.route("/api/ota/status", [&b](const Request&) { return otaStatus(b); });
    server.route("/api/ota/reboot", [&b](const Request& r) { return otaReboot(b, r); });
    server.route("/api/eventlogger/config", [&b](const Request& r) { return eventLogger(b, r); });
    server.route("/api/logs/data", [](const Request&) { return logData(); });
    server.route("/api/device/settings", [&b](const Request& r) { return deviceSettings(b, r); });
    server.route("/api/network/config", [&b](const Request& r) { return networkConfig(b, r); });
}

std::string telemetryPacket(const Status& s) {
    std::string p(32, '\0');  // little-endian, packed, as the firmware's struct on the Teensy
    const uint32_t ts = static_cast<uint32_t>(s.uptimeMs);
    const int16_t encoder = 0;  // no encoder on the HAT
    const uint16_t flags = s.autosteerActive ? 1 : 0;
    const uint8_t steer = s.steerSwitch, work = s.workSwitch, workPct = 0;
    put(p, 0, &ts, 4);
    put(p, 4, &s.wasAngle, 4);
    put(p, 8, &s.targetAngle, 4);
    put(p, 12, &encoder, 2);
    put(p, 14, &s.currentAmps, 4);
    put(p, 18, &s.speedKph, 4);
    put(p, 22, &s.heading, 4);
    put(p, 26, &flags, 2);
    put(p, 28, &steer, 1);
    put(p, 29, &work, 1);
    put(p, 30, &workPct, 1);
    return p;
}

std::string logHistoryJson() {
    static log::Entry entries[log::Logger::kHistory];
    const size_t n = log::global().drainedHistory(entries, log::Logger::kHistory);
    JsonDocument doc;
    doc["type"] = "history";
    putLogs(doc, entries, n);
    return toString(doc);
}

std::string logEntryJson(const log::Entry& e) {
    JsonDocument doc;
    doc["timestamp"] = e.timestampMs;
    doc["severity"] = static_cast<int>(e.severity);
    doc["source"] = static_cast<int>(e.source);
    doc["severityName"] = log::severityName(e.severity);
    doc["sourceName"] = log::sourceName(e.source);
    doc["message"] = e.message;
    return toString(doc);
}

}  // namespace aow::web
