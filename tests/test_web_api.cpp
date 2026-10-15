#include "api.h"

#include <ArduinoJson.h>

#include <cstdio>
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
    IsobusStatus iso;
    IsobusStatus isobusStatus() override { return iso; }
    void restart() override { ++restarts; }
    std::string updatePackage, updateJson;
    int reboots = 0;
    bool startUpdate(const std::string& p, std::string&) override { updatePackage = p; return true; }
    std::string updateStatus() override { return updateJson; }
    void reboot() override { ++reboots; }
    std::string canJson;
    std::string customCanConfig() override { return canJson; }
    bool writeCustomCanConfig(const std::string& j, std::string&) override { canJson = j; return true; }
    bool deleteCustomCanConfig(std::string&) override { canJson.clear(); return true; }
    Um98x::Config um;
    bool um98xRead(Um98x::Config& c, std::string&) override { c = um; return true; }
    bool um98xWrite(const Um98x::Config& c, std::string& e) override {
        if (c.modeSettings.empty()) { e = "no mode"; return false; }
        um = c;
        return true;
    }
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
    CHECK(d["isobus"]["state"] == "disabled" && d["isobus"]["address"].isNull());
    b.iso.state = IsobusStatus::State::Claimed;
    b.iso.channel = 2;
    b.iso.address = 247;
    b.iso.name = 0xA00082000AFE0014ull;
    d = parse(request(s, port, "GET /api/status HTTP/1.1\r\n\r\n"));
    CHECK(d["isobus"]["state"] == "claimed" && d["isobus"]["address"] == 247 && d["isobus"]["channel"] == 2);
    CHECK(d["isobus"]["name"] == "A00082000AFE0014" && d["isobus"]["busActive"] == false);

    // The ISOBUS page: settings and the same state; a save says whether a restart is needed.
    b.iso.sections = 3;
    std::snprintf(b.iso.implement, sizeof b.iso.implement, "Sprayer 24 m");
    std::snprintf(b.iso.interface, sizeof b.iso.interface, "hatcan2");
    b.iso.setpoint = 0b101;
    b.iso.actual = 0b001;
    b.iso.tecu = IsobusStatus::Tecu::Active;
    b.iso.speedMmS = 2000;
    d = parse(request(s, port, "GET /api/isobus HTTP/1.1\r\n\r\n"));
    CHECK(d["enabled"] == false && d["channel"] == 2 && d["tractorEcu"] == true && d["terminal"] == true);
    CHECK(d["status"]["terminal"] == "off");
    CHECK(d["status"]["state"] == "claimed" && d["status"]["taskController"]["setpoint"] == "101");
    CHECK(d["status"]["taskController"]["actual"] == "100" && d["status"]["tractorEcu"]["state"] == "active");
    // Strings are copied into the reply: the status they come from is a temporary.
    CHECK(d["status"]["taskController"]["implement"] == "Sprayer 24 m" && d["status"]["interface"] == "hatcan2");
    CHECK(d["status"]["tractorEcu"]["speed"].as<double>() > 7.19 && d["status"]["tractorEcu"]["speed"].as<double>() < 7.21);
    {
        const int applied = b.applied;
        r = post(s, port, "/api/isobus", R"({"enabled":true,"channel":3})");
        CHECK(parse(r)["restartRequired"] == true && b.applied == applied + 1);
        CHECK(b.s.isobus.enabled && b.s.isobus.channel == 3 && b.s.isobus.tractorEcu);
        r = post(s, port, "/api/isobus", R"({"enabled":true,"channel":9,"tractorEcu":"yes"})");  // bad values: ignored
        CHECK(parse(r)["restartRequired"] == false && b.s.isobus.channel == 3 && b.s.isobus.tractorEcu);
        r = post(s, port, "/api/isobus", R"({"terminal":false})");
        CHECK(parse(r)["restartRequired"] == true && !b.s.isobus.terminal);
        b.s.isobus.terminal = true;
        CHECK(request(s, port, "GET /isobus HTTP/1.1\r\n\r\n").find("/api/isobus") != std::string::npos);
        b.applied = applied;
    }

    // Device settings: GET shows the firmware field names.
    d = parse(request(s, port, "GET /api/device/settings HTTP/1.1\r\n\r\n"));
    CHECK(d["softStartDuration"] == 500 && d["serialRadioBaud"] == 115200 && d["buzzerVolume"] == 1);
    CHECK(d["ledBrightness"] == 25 && d["invertWorkSwitch"] == false);

    // POST: only the fields sent change; bad values are clamped or ignored.
    r = post(s, port, "/api/device/settings",
             R"({"softStartDuration":2500,"serialRadioBaud":12345,"buzzerVolume":2,"invertWorkSwitch":true,"ledBrightness":0})");
    CHECK(parse(r)["status"] == "saved" && b.applied == 1);
    CHECK(b.s.steer.softStartDurationMs == 1000 && b.s.ports.rate[0] == 0);
    CHECK(b.s.misc.buzzerVolume == 2 && b.s.misc.invertWorkSwitch && b.s.misc.ledBrightness == 5);
    CHECK(!b.s.gps.passThrough);
    post(s, port, "/api/device/settings", R"({"serialRadioBaud":38400})");
    CHECK(b.s.ports.rate[0] == 38400 && b.s.misc.buzzerVolume == 2);

    // Serial ports (/api/serial): the outputs and rates, live (never a restart); a bad value is
    // refused with a reason and nothing saved. The firmware's radio rate pins RS-232 #1.
    using P = Settings::Ports;
    d = parse(request(s, port, "GET /api/serial HTTP/1.1\r\n\r\n"));
    CHECK(d["nmeaOut"] == "none" && d["rtcmOut"] == "none" && d["rate"][0] == 38400 && d["rate"][1] == 0);
    CHECK(d["nmeaCorrected"] == true);
    CHECK(parse(request(s, port, "GET /api/device/settings HTTP/1.1\r\n\r\n"))["serialRadioBaud"] == 38400);
    r = post(s, port, "/api/serial", R"({"nmeaOut":"port2","rtcmOut":"port2","rate":[0,9600],"nmeaRateHz":5})");
    CHECK(parse(r)["restartRequired"] == false && b.s.ports.nmeaOut == P::kPort2 && b.s.ports.rtcmOut == P::kPort2);
    CHECK(b.s.ports.rate[0] == 0 && b.s.ports.rate[1] == 9600 && b.s.ports.nmeaRateHz == 5);
    CHECK(b.s.ports.nmeaCorrected);
    r = post(s, port, "/api/serial", R"({"nmeaCorrected":false})");
    CHECK(parse(r)["status"] == "saved" && !b.s.ports.nmeaCorrected && b.s.ports.nmeaOut == P::kPort2);
    const int beforeBad = b.applied;
    r = post(s, port, "/api/serial", R"({"nmeaOut":"usb"})");  // no device named
    CHECK(r.rfind("HTTP/1.1 400", 0) == 0 && !parse(r)["message"].as<std::string>().empty());
    CHECK(b.applied == beforeBad && b.s.ports.nmeaOut == P::kPort2);
    CHECK(post(s, port, "/api/serial", R"({"rtcmOut":"somewhere"})").rfind("HTTP/1.1 400", 0) == 0);
    CHECK(post(s, port, "/api/serial", R"({"rate":[14400,0]})").rfind("HTTP/1.1 400", 0) == 0);
    r = post(s, port, "/api/serial", R"({"nmeaOut":"usb","nmeaOutUsb":"usb-FTDI_FT232R_USB_UART_A1-if00-port0"})");
    CHECK(parse(r)["status"] == "saved" && b.s.ports.nmeaOut == P::kUsb && !std::strcmp(b.s.ports.nmeaOutUsb, "usb-FTDI_FT232R_USB_UART_A1-if00-port0"));
    post(s, port, "/api/device/settings", R"({"serialRadioBaud":57600})");
    CHECK(b.s.ports.rate[0] == 57600);
    r = post(s, port, "/api/serial", R"({"nmeaOut":"none","rtcmOut":"none","rate":[38400,0]})");
    CHECK(parse(r)["status"] == "saved");
    CHECK(request(s, port, "GET /serial HTTP/1.1\r\n\r\n").find("/api/serial") != std::string::npos);

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

    // CAN: the pages, settings in the firmware's field names, duplicate bus names refused.
    CHECK(request(s, port, "GET /can HTTP/1.1\r\n\r\n").find("/api/can/info") != std::string::npos);
    CHECK(request(s, port, "GET /can/upload HTTP/1.1\r\n\r\n").find("/api/can/config/upload") != std::string::npos);
    d = parse(request(s, port, "GET /api/can/config HTTP/1.1\r\n\r\n"));
    CHECK(d["brand"] == 0 && d["moduleID"] == 0x1C && d["can3Function"] == 0);
    d = parse(post(s, port, "/api/can/config", R"({"brand":9,"can3Function":1,"can3Name":1,"can1Function":2,"can1Speed":1})"));
    CHECK(d["status"] == "ok" && b.s.can.brand == 9 && b.s.can.bus[2].function == 1 && b.s.can.bus[0].speed == 1);
    d = parse(post(s, port, "/api/can/config", R"({"can1Name":1})"));  // CAN3 already has name 1
    CHECK(d["status"] == "error" && b.s.can.bus[0].name == 0);

    // Brand file: the built-in list until one is uploaded; bad JSON refused; restore removes it.
    d = parse(request(s, port, "GET /api/can/info HTTP/1.1\r\n\r\n"));
    CHECK(d["brands"].size() >= 2);
    d = parse(request(s, port, "GET /api/can/config/status HTTP/1.1\r\n\r\n"));
    CHECK(d["custom"] == false);
    CHECK(post(s, port, "/api/can/config/upload", "{\"brands\": [").rfind("HTTP/1.1 400", 0) == 0 && b.canJson.empty());
    CHECK(body(post(s, port, "/api/can/config/upload", R"({"version":"2.1","brands":[{"id":9}]})")) == "Upload successful");
    d = parse(request(s, port, "GET /api/can/config/status HTTP/1.1\r\n\r\n"));
    CHECK(d["custom"] == true && d["version"] == "2.1" && d["size"] > 10);
    d = parse(request(s, port, "GET /api/can/info HTTP/1.1\r\n\r\n"));
    CHECK(d["version"] == "2.1");
    CHECK(body(post(s, port, "/api/can/config/restore", "")) == "Default configuration restored" && b.canJson.empty());

    // GPS (UM98x) page and API.
    CHECK(request(s, port, "GET /um98x-config HTTP/1.1\r\n\r\n").find("/api/um98x/read") != std::string::npos);
    CHECK(request(s, port, "GET /gps HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 200", 0) == 0);
    d = parse(post(s, port, "/api/um98x/write", R"({"config":"CONFIG COM1 460800","mode":"MODE ROVER","messages":"GPGGA COM1 0.1"})"));
    CHECK(d["success"] == true && b.um.modeSettings == "MODE ROVER");
    d = parse(request(s, port, "GET /api/um98x/read HTTP/1.1\r\n\r\n"));
    CHECK(d["success"] == true && d["config"] == "CONFIG COM1 460800" && d["messages"] == "GPGGA COM1 0.1");
    d = parse(post(s, port, "/api/um98x/write", R"({"config":"x"})"));
    CHECK(d["success"] == false && d["error"] == "no mode");

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

    // System update: only a package-shaped file, only with steering off; status passes through.
    std::string pkg = std::string("\x1f\x8b", 2) + std::string(200, 'x') + std::string(64, 's') + "AOWHAT1\n";
    CHECK(looksLikeUpdatePackage(pkg) && !looksLikeUpdatePackage(pkg.substr(1)) && !looksLikeUpdatePackage(":10000000"));
    CHECK(request(s, port, "GET /ota HTTP/1.1\r\n\r\n").find(".aowpkg") != std::string::npos);
    CHECK(request(s, port, "GET /api/ota/upload HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 405", 0) == 0);
    CHECK(post(s, port, "/api/ota/upload", "not a package").rfind("HTTP/1.1 400", 0) == 0 && b.updatePackage.empty());
    b.st.autosteerActive = true;
    CHECK(post(s, port, "/api/ota/upload", pkg).rfind("HTTP/1.1 409", 0) == 0 && b.updatePackage.empty());
    CHECK(post(s, port, "/api/ota/reboot", "").rfind("HTTP/1.1 409", 0) == 0 && b.reboots == 0);
    b.st.autosteerActive = false;
    CHECK(post(s, port, "/api/ota/upload", pkg).rfind("HTTP/1.1 200", 0) == 0 && b.updatePackage == pkg);
    CHECK(parse(request(s, port, "GET /api/ota/status HTTP/1.1\r\n\r\n"))["state"] == "idle");
    b.updateJson = R"({"state":"done","version":"9.9.9"})";
    CHECK(parse(request(s, port, "GET /api/ota/status HTTP/1.1\r\n\r\n"))["version"] == "9.9.9");
    CHECK(parse(post(s, port, "/api/ota/reboot", ""))["status"] == "rebooting" && b.reboots == 1);

    return finish("web_api");
}
