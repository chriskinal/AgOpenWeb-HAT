// The firmware's web API (SimpleWebManager) on top of aow::web::Server. Request and response
// fields keep the firmware's names so its pages work unchanged.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aow/helper_status.h"
#include "aow/log.h"
#include "aow/settings.h"
#include "aow/status.h"
#include "aow/um98x.h"
#include "server.h"

namespace aow::web {

// What the API needs from the daemon. Called on the web thread; implementations lock as needed.
struct Backend {
    virtual ~Backend() = default;
    virtual Settings settings() = 0;
    // Saves and applies. Returns false with a reason if the settings could not be saved.
    virtual bool applySettings(const Settings& s, std::string& error) = 0;
    virtual Status status() = 0;
    virtual IsobusStatus isobusStatus() { return {}; }
    // The AOW link gateway's last report (aow-linkd), if one came within helper::kStaleMs.
    virtual bool linkGateway(helper::LinkGateway&) { return false; }
    // Its service: whether it is installed and switched on (enabled), and switching it, which
    // starts or stops it at once. Nothing else restarts.
    virtual bool linkGatewayInstalled() { return false; }
    virtual bool linkGatewayEnabled() { return false; }
    virtual bool setLinkGatewayEnabled(bool, std::string& error) { error = "not available"; return false; }
    // Its settings on the ISOBUS page: the name it gives itself (its HELLO and its announcement on
    // the network; empty: the default) and whether tablets on the network may connect (it listens
    // beyond loopback, and announces itself by DNS-SD while it is on). Saving restarts it if it runs.
    struct LinkGatewaySettings { std::string name; bool network = false; };
    virtual LinkGatewaySettings linkGatewaySettings() { return {}; }
    virtual std::string linkGatewayDefaultName() { return ""; }
    virtual bool linkGatewayAnnounced() { return false; }
    virtual bool setLinkGatewaySettings(const LinkGatewaySettings&, std::string& error) { error = "not available"; return false; }
    virtual void restart() = 0;  // restart the daemon (the firmware rebooted the Teensy)

    // The uploaded CAN brand file (the firmware's LittleFS /can_config.json). "" if none.
    virtual std::string customCanConfig() = 0;
    virtual bool writeCustomCanConfig(const std::string& json, std::string& error) = 0;
    virtual bool deleteCustomCanConfig(std::string& error) = 0;

    // UM98x receiver setup (blocks for seconds; the web thread may).
    virtual bool um98xRead(Um98x::Config& c, std::string& error) = 0;
    virtual bool um98xWrite(const Um98x::Config& c, std::string& error) = 0;

    // System update (the firmware's OTA page). The package is a signed bundle from
    // deploy/package.sh; a separate root service verifies and installs it.
    virtual bool startUpdate(const std::string& package, std::string& error) = 0;
    virtual std::string updateStatus() = 0;  // the updater's status JSON; "" if it never ran
    virtual void reboot() = 0;               // restart the whole unit

    struct Interface { std::string name, ip; bool up; int speedMbps; };
    virtual std::vector<Interface> interfaces() = 0;
};

// Ports the pages expect: HTTP 80, telemetry WebSocket 8082, log WebSocket 8083.
constexpr uint16_t kHttpPort = 80;
constexpr uint16_t kTelemetryPort = 8082;
constexpr uint16_t kLogPort = 8083;

void addRoutes(Server& server, Backend& backend, const char* version);

// True if the bytes have the shape of an update package: a gzip stream, a 64-byte signature and
// the trailer "AOWHAT1\n". The signature itself is checked by the updater, not here.
bool looksLikeUpdatePackage(const std::string& bytes);
constexpr size_t kMaxUpdatePackage = 16 * 1024 * 1024;

// The firmware's 32-byte little-endian TelemetryPacket, sent on 8082.
std::string telemetryPacket(const Status& s);

// One log entry as the firmware's log WebSocket sends it on 8083.
std::string logEntryJson(const log::Entry& e);
// AOW HAT: the entries already sent, for a viewer that has just connected. The firmware's page
// handles this message ({"type":"history","logs":[...]}) though the firmware never sends it.
std::string logHistoryJson();

}  // namespace aow::web
