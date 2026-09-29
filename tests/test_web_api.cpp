#include "api.h"

#include <ArduinoJson.h>

#include <cstring>

#include "check.h"
#include "http_client.h"

using namespace aow;
using namespace aow::web;

struct FakeBackend : Backend {
    Settings s;
    Status st;
    int applied = 0, restarts = 0;
    bool failSave = false;
    Settings settings() override { return s; }
    bool applySettings(const Settings& n, std::string& err) override {
        if (failSave) { err = "disk full"; return false; }
        s = n;
        ++applied;
        return true;
    }
    Status status() override { return st; }
    void restart() override { ++restarts; }
    std::vector<Interface> interfaces() override {
        return {{"lo", "127.0.0.1", true, 0}, {"wlan0", "192.168.4.217", true, 0}, {"eth0", "", false, 0}};
    }
};

static std::string body(const std::string& resp) {
    const size_t p = resp.find("\r\n\r\n");
    return p == std::string::npos ? "" : resp.substr(p + 4);
}

static std::string post(Server& s, uint16_t port, const char* path, const std::string& json) {
    return request(s, port, std::string("POST ") + path + " HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: " +
                                std::to_string(json.size()) + "\r\n\r\n" + json);
}

static JsonDocument parse(const std::string& resp) {
    JsonDocument d;
    deserializeJson(d, body(resp));
    return d;
}

int main() {
    FakeBackend b;
    Server s;
    const uint16_t port = s.listenHttp(0);
    addRoutes(s, b, "0.1.0");

    // Pages are served, with the HAT's changes.
    std::string r = request(s, port, "GET / HTTP/1.1\r\n\r\n");
    CHECK(r.rfind("HTTP/1.1 200", 0) == 0 && r.find("AgOpenWeb HAT") != std::string::npos);
    CHECK(r.find("/analogworkswitch") == std::string::npos);
    r = request(s, port, "GET /device HTTP/1.1\r\n\r\n");
    CHECK(r.find("invertWorkSwitch") != std::string::npos && r.find("jdPWMEnabled") == std::string::npos);
    CHECK(request(s, port, "GET /touch.css HTTP/1.1\r\n\r\n").find("Content-Type: text/css") != std::string::npos);
    for (const char* p : {"/eventlogger", "/logs", "/network"})
        CHECK(request(s, port, std::string("GET ") + p + " HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 200", 0) == 0);

    // Status: the first interface that is up, skipping loopback's place in the list is not
    // required, but the pages show one IP; the full list is there too.
    b.st.uptimeMs = 12345;
    b.st.fixQuality = 4;
    JsonDocument d = parse(request(s, port, "GET /api/status HTTP/1.1\r\n\r\n"));
    CHECK(d["version"] == "0.1.0" && d["uptime"] == 12345 && d["moduleId"] == 126);
    CHECK(d["network"]["interfaces"].size() == 3 && d["gnss"]["fixQuality"] == 4);

    // Device settings: GET shows the firmware field names.
    d = parse(request(s, port, "GET /api/device/settings HTTP/1.1\r\n\r\n"));
    CHECK(d["softStartDuration"] == 500 && d["serialRadioBaud"] == 115200 && d["buzzerVolume"] == 1);
    CHECK(d["ledBrightness"] == 25 && d["invertWorkSwitch"] == false);

    // POST: only the fields sent change; bad values are clamped or ignored.
    r = post(s, port, "/api/device/settings",
             R"({"softStartDuration":2500,"serialRadioBaud":12345,"buzzerVolume":2,"invertWorkSwitch":true,"ledBrightness":0})");
    CHECK(parse(r)["status"] == "saved" && b.applied == 1);
    CHECK(b.s.steer.softStartDurationMs == 1000 && b.s.gps.radioBaud == 115200);
    CHECK(b.s.misc.buzzerVolume == 2 && b.s.misc.invertWorkSwitch && b.s.misc.ledBrightness == 5);
    CHECK(!b.s.gps.passThrough);
    post(s, port, "/api/device/settings", R"({"serialRadioBaud":38400})");
    CHECK(b.s.gps.radioBaud == 38400 && b.s.misc.buzzerVolume == 2);

    // Broken JSON and a failed save are reported, not applied.
    CHECK(post(s, port, "/api/device/settings", "{nope").rfind("HTTP/1.1 400", 0) == 0);
    b.failSave = true;
    r = post(s, port, "/api/device/settings", R"({"buzzerVolume":0})");
    CHECK(r.rfind("HTTP/1.1 500", 0) == 0 && parse(r)["message"] == "disk full" && b.s.misc.buzzerVolume == 2);
    b.failSave = false;

    // Event logger: numeric levels, as the page sends them.
    d = parse(request(s, port, "GET /api/eventlogger/config HTTP/1.1\r\n\r\n"));
    CHECK(d["serialLevel"] == 6 && d["udpLevel"] == 4 && d["serialEnabled"] == true);
    post(s, port, "/api/eventlogger/config", R"({"serialLevel":7,"udpEnabled":true,"udpLevel":9})");
    CHECK(b.s.log.consoleLevel == log::Severity::Debug && b.s.log.udpEnabled);
    CHECK(b.s.log.udpLevel == log::Severity::Warning);  // 9 is out of range: unchanged

    // Restart only on POST.
    CHECK(request(s, port, "GET /api/restart HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 405", 0) == 0 && b.restarts == 0);
    CHECK(parse(post(s, port, "/api/restart", ""))["status"] == "restarting" && b.restarts == 1);

    // Network: GET gives the first up address as four numbers; POST explains, changes nothing.
    d = parse(request(s, port, "GET /api/network/config HTTP/1.1\r\n\r\n"));
    CHECK(d["ip"].size() == 4 && d["ip"][0] == 127);
    CHECK(parse(post(s, port, "/api/network/config", R"({"ip":[10,0,0]})"))["status"] == "error");

    // Telemetry packet: 32 bytes, the firmware's offsets.
    Status st;
    st.uptimeMs = 0x01020304;
    st.wasAngle = 1.5f;
    st.targetAngle = -2.25f;
    st.currentAmps = 3.0f;
    st.speedKph = 8.0f;
    st.heading = 271.0f;
    st.autosteerActive = true;
    st.workSwitch = true;
    const std::string p = telemetryPacket(st);
    float f;
    uint32_t ts;
    CHECK(p.size() == 32);
    std::memcpy(&ts, &p[0], 4);
    CHECK(ts == 0x01020304);
    std::memcpy(&f, &p[4], 4);  CHECK(f == 1.5f);
    std::memcpy(&f, &p[8], 4);  CHECK(f == -2.25f);
    std::memcpy(&f, &p[14], 4); CHECK(f == 3.0f);
    std::memcpy(&f, &p[18], 4); CHECK(f == 8.0f);
    std::memcpy(&f, &p[22], 4); CHECK(f == 271.0f);
    CHECK(p[26] == 1 && p[27] == 0 && p[28] == 0 && p[29] == 1);

    // Log entries: the firmware's JSON, with quotes escaped.
    log::Entry e;
    e.timestampMs = 42;
    e.severity = log::Severity::Warning;
    e.source = log::Source::Can;
    std::strcpy(e.message, "valve \"not\" ready");
    JsonDocument le;
    CHECK(!deserializeJson(le, logEntryJson(e)));
    CHECK(le["timestamp"] == 42 && le["severity"] == 4 && le["source"] == 6);
    CHECK(le["severityName"] == "WARN" && le["sourceName"] == "CAN" && le["message"] == "valve \"not\" ready");

    return finish("web_api");
}
