#include "api.h"

#define PROGMEM  // the firmware's pages are Arduino flash strings; plain const here

#include <ArduinoJson.h>

#include <cstring>

#include "pages/TouchFriendlyDeviceSettingsPage.h"
#include "pages/TouchFriendlyEventLoggerPage.h"
#include "pages/TouchFriendlyHomePage.h"
#include "pages/TouchFriendlyLogViewerPage.h"
#include "pages/TouchFriendlyNetworkPage.h"
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

Response logData() {
    static log::Entry entries[log::Logger::kHistory];
    const size_t n = log::global().history(entries, log::Logger::kHistory);
    JsonDocument doc;
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
    return Response::json(toString(doc));
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
        doc["serialRadioBaud"] = s.gps.radioBaud;
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
    if (doc["serialRadioBaud"].is<uint32_t>()) {
        static const uint32_t kBauds[] = {4800, 9600, 14400, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
        const uint32_t v = doc["serialRadioBaud"];
        for (uint32_t ok : kBauds)
            if (v == ok) s.gps.radioBaud = v;
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
    return saveOr500(b, s, R"({"status":"saved"})");
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

void put(std::string& out, size_t at, const void* v, size_t n) { std::memcpy(&out[at], v, n); }

}  // namespace

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

    server.route("/api/status", [&b, version](const Request&) { return status(b, version); });
    server.route("/api/restart", [&b](const Request& r) {
        if (r.method != "POST") return methodNotAllowed();
        b.restart();
        return Response::json(R"({"status":"restarting"})");
    });
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
