// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors

#include "aow/wifi.h"

#include <algorithm>

#include "check.h"

using namespace aow;
using namespace aow::wifi;

static bool has(const Args& a, const std::string& key, const std::string& value) {
    for (size_t i = 0; i + 1 < a.size(); ++i)
        if (a[i] == key && a[i + 1] == value) return true;
    return false;
}
static bool hasWord(const Args& a, const std::string& w) { return std::find(a.begin(), a.end(), w) != a.end(); }

int main() {
    // What a request may say.
    Request ap;
    ap.mode = Mode::AccessPoint;
    ap.ssid = "AgOpenWeb-cab";
    ap.password = "tractor123";
    CHECK(requestError(ap, false, "US").empty());
    Request r = ap;
    r.password = "";
    CHECK(!requestError(r, false, "US").empty());  // a new access point needs a password
    CHECK(requestError(r, true, "US").empty());    // a saved one keeps its own
    r.password = "short";
    CHECK(requestError(r, true, "US") == "The password must be 8 to 63 characters");
    r.password = std::string(64, 'a');
    CHECK(!requestError(r, true, "US").empty());
    r.password = "caf\xc3\xa9-password";
    CHECK(!requestError(r, true, "US").empty());  // ASCII only
    r = ap;
    r.ssid = "";
    CHECK(requestError(r, false, "US") == "Enter the network name (SSID)");
    r.ssid = std::string(33, 'x');
    CHECK(!requestError(r, false, "US").empty());
    r.ssid = "line\nbreak";
    CHECK(!requestError(r, false, "US").empty());
    r = ap;
    r.channel = 12;
    CHECK(!requestError(r, false, "US").empty());
    r.channel = 0;
    CHECK(!requestError(r, false, "US").empty());
    r.band5 = true;
    r.channel = 36;
    CHECK(requestError(r, false, "US").empty());
    CHECK(!requestError(r, false, "00").empty());  // 5 GHz needs the country
    CHECK(!requestError(r, false, "").empty());
    r.channel = 52;  // radar detection: not offered
    CHECK(!requestError(r, false, "US").empty());
    // A client may join an open network, and its channel is not looked at.
    Request client;
    client.ssid = "Dry-Kreek";
    client.channel = 0;
    CHECK(requestError(client, false, "00").empty());

    CHECK(defaultApSsid("agopenweb") == "AgOpenWeb-agopenweb");
    CHECK(defaultApSsid("") == "AgOpenWeb-HAT");
    CHECK(defaultApSsid(std::string(40, 'h')).size() == 32);

    // The access point's profile: made or changed, autoconnect off until it is up, the
    // password only if given.
    Args a = apProfileArgs(ap, false);
    CHECK(a[0] == "nmcli" && a[1] == "connection" && a[2] == "add" && has(a, "con-name", kApProfile));
    CHECK(has(a, "ifname", "wlan0") && has(a, "802-11-wireless.mode", "ap") && has(a, "802-11-wireless.ssid", "AgOpenWeb-cab"));
    CHECK(has(a, "connection.autoconnect", "no") && has(a, "connection.autoconnect-priority", "100"));
    CHECK(has(a, "802-11-wireless.band", "bg") && has(a, "802-11-wireless.channel", "6"));
    CHECK(has(a, "ipv4.method", "shared") && has(a, "ipv4.addresses", "10.42.0.1/24"));
    CHECK(has(a, "wifi-sec.key-mgmt", "wpa-psk") && has(a, "wifi-sec.psk", "tractor123") && has(a, "wifi-sec.pmf", "disable"));
    r = ap;
    r.password = "";
    r.band5 = true;
    r.channel = 44;
    a = apProfileArgs(r, true);
    CHECK(a[2] == "modify" && a[3] == kApProfile && !hasWord(a, "con-name"));
    CHECK(has(a, "802-11-wireless.band", "a") && has(a, "802-11-wireless.channel", "44") && !hasWord(a, "wifi-sec.psk"));
    // An SSID with spaces and quotes is one argument, never parsed by a shell.
    r.ssid = "Bob's \"cab\" $(reboot)";
    a = apProfileArgs(r, true);
    CHECK(has(a, "802-11-wireless.ssid", "Bob's \"cab\" $(reboot)"));

    // A client network: made (named by its SSID) or changed by its uuid.
    client.password = "hunter2hunter2";
    a = clientProfileArgs(client, "");
    CHECK(a[2] == "add" && has(a, "con-name", "Dry-Kreek") && has(a, "802-11-wireless.ssid", "Dry-Kreek"));
    CHECK(has(a, "connection.autoconnect", "yes") && has(a, "wifi-sec.psk", "hunter2hunter2"));
    a = clientProfileArgs(client, "1a3c-uuid");
    CHECK(a[2] == "modify" && has(a, "uuid", "1a3c-uuid") && has(a, "wifi-sec.psk", "hunter2hunter2") && !hasWord(a, "con-name"));
    client.password = "";
    a = clientProfileArgs(client, "");
    CHECK(a[2] == "add" && !hasWord(a, "wifi-sec.key-mgmt"));  // an open network
    a = clientProfileArgs(client, "1a3c-uuid");
    CHECK(a[2] == "modify" && !hasWord(a, "wifi-sec.psk") && !hasWord(a, "wifi-sec.key-mgmt"));

    a = upArgs("u-1", 45);
    CHECK(a == (Args{"nmcli", "--wait", "45", "connection", "up", "uuid", "u-1"}));
    CHECK(has(apAutoconnectArgs(true), "connection.autoconnect", "yes"));
    CHECK(has(apAutoconnectArgs(false), "connection.autoconnect", "no"));

    // nmcli's terse output.
    auto f = splitTerse(R"(uuid-1:802-11-wireless:yes:Cab\: north \\ shed)");
    CHECK(f.size() == 4 && f[2] == "yes" && f[3] == "Cab: north \\ shed");
    CHECK(splitTerse("").size() == 1 && splitTerse("a::b").size() == 3);
    const std::string detail = "802-11-wireless.ssid:Dry:Kreek\n802-11-wireless.mode:infrastructure\nIP4.ADDRESS[1]:192.168.4.217/24\n";
    CHECK(field(detail, "802-11-wireless.ssid") == "Dry:Kreek");
    CHECK(field(detail, "802-11-wireless.mode") == "infrastructure");
    CHECK(field(detail, "IP4.ADDRESS[1]") == "192.168.4.217/24");
    CHECK(field(detail, "802-11-wireless").empty() && field(detail, "GENERAL.STATE").empty());

    // A scan: strongest first, each SSID once, hidden ones left out.
    auto seen = parseScan("Dry-Kreek:63:WPA2\n:80:WPA2\nNeighbour:40:\nDry-Kreek:71:WPA2\nCab\\:1:90:WPA1 WPA2\n");
    CHECK(seen.size() == 3);
    CHECK(seen[0].ssid == "Cab:1" && seen[0].signal == 90 && seen[0].security == "WPA1 WPA2");
    CHECK(seen[1].ssid == "Dry-Kreek" && seen[1].signal == 71);
    CHECK(seen[2].ssid == "Neighbour" && seen[2].security.empty());

    return finish("wifi");
}
