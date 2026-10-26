#include "aow/helper_status.h"

#include <cstring>
#include <string>

#include "check.h"

using namespace aow::helper;

int main() {
    // As aow-linkd sends it (SharpIsoTerm gateway/linux, --status), with a terminal connected.
    const std::string report =
        R"({"gateway":"aow-linkd","version":"0.2.30","name":"AOW HAT aowhat","listen":"0.0.0.0","port":2323,)"
        R"("connections":3,"monitors":1,"client":{"name":"isoshim bench on macOS 26.6.2","address":"192.168.4.227",)"
        R"("toClient":24439,"toBus":314,"txDropped":2,"rxDropped":16909060},"channels":[)"
        R"({"interface":"hatcan2","bitrate":250000,"state":0,"txErrors":0,"rxErrors":0},)"
        R"({"interface":"hatcan3","bitrate":0,"state":3,"txErrors":255,"rxErrors":7}]})";
    LinkGateway d;
    CHECK(decodeLinkGateway(report.data(), report.size(), d));
    CHECK(d.clientConnected && d.allNetworks && d.port == 2323 && d.monitors == 1 && d.connections == 3);
    CHECK(d.toClient == 24439 && d.toBus == 314 && d.txDropped == 2 && d.rxDropped == 0x01020304);
    CHECK(!std::strcmp(d.client, "isoshim bench on macOS 26.6.2") && !std::strcmp(d.clientAddress, "192.168.4.227"));
    CHECK(d.channelCount == 2 && !std::strcmp(d.channels[0].interface, "hatcan2") && d.channels[0].bitrate == 250000);
    CHECK(d.channels[1].state == 3 && d.channels[1].txErrors == 255 && d.channels[1].rxErrors == 7);
    CHECK(!std::strcmp(busStateName(d.channels[1].state), "bus-off") && !std::strcmp(busStateName(9), "down"));

    // Not JSON, cut short, or another helper's: refused, and d untouched.
    CHECK(!decodeLinkGateway("\x80\x81\x81\x99", 4, d) && d.port == 2323);
    CHECK(!decodeLinkGateway(report.data(), report.size() - 3, d) && d.port == 2323);
    const std::string other = R"({"gateway":"aow-tcd","port":1})";
    CHECK(!decodeLinkGateway(other.data(), other.size(), d) && d.port == 2323);

    // Nothing connected, on loopback: empty strings, zero counts.
    const std::string idle = R"({"gateway":"aow-linkd","listen":"127.0.0.1","port":2323,"connections":0,)"
                             R"("monitors":0,"client":null,"channels":[]})";
    CHECK(decodeLinkGateway(idle.data(), idle.size(), d) && !d.clientConnected && !d.allNetworks && d.client[0] == 0 &&
          d.toClient == 0 && d.channelCount == 0);

    // A name too long for the field is cut, not overrun; channels past four are left out.
    std::string lng = R"({"gateway":"aow-linkd","client":{"name":")" + std::string(100, 'x') + R"("},"channels":[)";
    for (int i = 0; i < 6; ++i) lng += std::string(i ? "," : "") + R"({"interface":"can)" + std::to_string(i) + R"("})";
    lng += "]}";
    CHECK(decodeLinkGateway(lng.data(), lng.size(), d) && std::strlen(d.client) == sizeof d.client - 1 &&
          d.channelCount == LinkGateway::kMaxChannels && !std::strcmp(d.channels[3].interface, "can3"));

    return finish("helper_status");
}
