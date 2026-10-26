#include "aow/helper_status.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <string_view>

namespace aow::helper {

namespace {

void copy(char* out, size_t size, const char* in) { std::snprintf(out, size, "%s", in ? in : ""); }

}  // namespace

// {"gateway":"aow-linkd","version":..,"name":..,"listen":"127.0.0.1","port":2323,"connections":1,
//  "monitors":0,"client":{"name","address","toClient","toBus","txDropped","rxDropped"} or null,
//  "channels":[{"interface","bitrate","state","txErrors","rxErrors"}]}
bool decodeLinkGateway(const char* json, size_t len, LinkGateway& s) {
    JsonDocument doc;
    if (deserializeJson(doc, json, len) || doc["gateway"] != "aow-linkd") return false;
    LinkGateway d;
    const char* listen = doc["listen"] | "127.0.0.1";
    d.allNetworks = std::string_view(listen) != "127.0.0.1";
    d.port = doc["port"] | 0;
    d.monitors = doc["monitors"] | 0;
    d.connections = doc["connections"] | 0;
    const JsonObjectConst client = doc["client"];
    if (client) {
        d.clientConnected = true;
        copy(d.client, sizeof d.client, client["name"]);
        copy(d.clientAddress, sizeof d.clientAddress, client["address"]);
        d.toClient = client["toClient"] | 0u;
        d.toBus = client["toBus"] | 0u;
        d.txDropped = client["txDropped"] | 0u;
        d.rxDropped = client["rxDropped"] | 0u;
    }
    for (const JsonObjectConst c : doc["channels"].as<JsonArrayConst>()) {
        if (d.channelCount == LinkGateway::kMaxChannels) break;
        LinkGateway::Channel& out = d.channels[d.channelCount++];
        copy(out.interface, sizeof out.interface, c["interface"]);
        out.bitrate = c["bitrate"] | 0u;
        out.state = c["state"] | 4;
        out.txErrors = c["txErrors"] | 0;
        out.rxErrors = c["rxErrors"] | 0;
    }
    s = d;
    return true;
}

const char* busStateName(uint8_t state) {
    switch (state) {
    case 0: return "error active";
    case 1: return "error warning";
    case 2: return "error passive";
    case 3: return "bus-off";
    default: return "down";
    }
}

}  // namespace aow::helper
