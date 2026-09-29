// The firmware's web API (SimpleWebManager) on top of aow::web::Server. Request and response
// fields keep the firmware's names so its pages work unchanged.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aow/log.h"
#include "aow/settings.h"
#include "aow/status.h"
#include "server.h"

namespace aow::web {

// What the API needs from the daemon. Called on the web thread; implementations lock as needed.
struct Backend {
    virtual ~Backend() = default;
    virtual Settings settings() = 0;
    // Saves and applies. Returns false with a reason if the settings could not be saved.
    virtual bool applySettings(const Settings& s, std::string& error) = 0;
    virtual Status status() = 0;
    virtual void restart() = 0;  // restart the daemon (the firmware rebooted the Teensy)

    // The uploaded CAN brand file (the firmware's LittleFS /can_config.json). "" if none.
    virtual std::string customCanConfig() = 0;
    virtual bool writeCustomCanConfig(const std::string& json, std::string& error) = 0;
    virtual bool deleteCustomCanConfig(std::string& error) = 0;

    struct Interface { std::string name, ip; bool up; int speedMbps; };
    virtual std::vector<Interface> interfaces() = 0;
};

// Ports the pages expect: HTTP 80, telemetry WebSocket 8082, log WebSocket 8083.
constexpr uint16_t kHttpPort = 80;
constexpr uint16_t kTelemetryPort = 8082;
constexpr uint16_t kLogPort = 8083;

void addRoutes(Server& server, Backend& backend, const char* version);

// The firmware's 32-byte little-endian TelemetryPacket, sent on 8082.
std::string telemetryPacket(const Status& s);

// One log entry as the firmware's log WebSocket sends it on 8083.
std::string logEntryJson(const log::Entry& e);

}  // namespace aow::web
