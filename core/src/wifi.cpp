#include "aow/wifi.h"

#include <algorithm>
#include <cstdlib>

namespace aow::wifi {

namespace {

bool printable(const std::string& s) {
    for (char c : s)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return false;
    return true;
}

}  // namespace

bool validChannel(bool band5, uint8_t channel) {
    if (band5) return channel == 36 || channel == 40 || channel == 44 || channel == 48;
    return channel >= 1 && channel <= 11;
}

std::string requestError(const Request& r, bool passwordKnown, const std::string& country) {
    if (r.ssid.empty()) return "Enter the network name (SSID)";
    if (r.ssid.size() > 32) return "The network name is too long (at most 32 bytes)";
    if (!printable(r.ssid)) return "The network name has a character that cannot be used";
    if (!r.password.empty()) {
        if (r.password.size() < 8 || r.password.size() > 63) return "The password must be 8 to 63 characters";
        for (char c : r.password)
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E)
                return "The password must be plain letters, digits and symbols (ASCII)";
    }
    if (r.mode == Mode::AccessPoint) {
        if (r.password.empty() && !passwordKnown) return "Set a password for the access point (8 to 63 characters)";
        if (!validChannel(r.band5, r.channel))
            return r.band5 ? "5 GHz channel must be 36, 40, 44 or 48" : "2.4 GHz channel must be 1 to 11";
        if (r.band5 && (country.empty() || country == "00"))
            return "5 GHz needs the Wi-Fi country set on this unit (raspi-config); use 2.4 GHz";
    }
    return "";
}

std::string defaultApSsid(const std::string& hostname) {
    std::string s = "AgOpenWeb-" + (hostname.empty() ? std::string("HAT") : hostname);
    return s.size() > 32 ? s.substr(0, 32) : s;
}

Args apProfileArgs(const Request& r, bool exists) {
    Args a = exists ? Args{"nmcli", "connection", "modify", kApProfile}
                    : Args{"nmcli", "connection", "add", "type", "wifi", "ifname", kDevice, "con-name", kApProfile};
    const Args common = {
        "connection.autoconnect", "no",
        "connection.autoconnect-priority", std::to_string(kApPriority),
        "802-11-wireless.ssid", r.ssid,
        "802-11-wireless.mode", "ap",
        "802-11-wireless.band", r.band5 ? "a" : "bg",
        "802-11-wireless.channel", std::to_string(r.channel),
        "ipv4.method", "shared",  // NetworkManager's DHCP and DNS for the tablets (dnsmasq)
        "ipv4.addresses", std::string(kApAddress) + "/" + std::to_string(kApPrefix),
        "ipv6.method", "disabled",
        // WPA2 with AES only, and no protected management frames: what the CM4's chip
        // (brcmfmac) does reliably as an access point, and what every tablet joins.
        "wifi-sec.key-mgmt", "wpa-psk",
        "wifi-sec.proto", "rsn",
        "wifi-sec.pairwise", "ccmp",
        "wifi-sec.group", "ccmp",
        "wifi-sec.pmf", "disable",
    };
    a.insert(a.end(), common.begin(), common.end());
    if (!r.password.empty()) {
        a.push_back("wifi-sec.psk");
        a.push_back(r.password);
    }
    return a;
}

Args clientProfileArgs(const Request& r, const std::string& uuid) {
    Args a = uuid.empty() ? Args{"nmcli", "connection", "add", "type", "wifi", "ifname", kDevice, "con-name", r.ssid,
                                 "802-11-wireless.ssid", r.ssid, "connection.autoconnect", "yes"}
                          : Args{"nmcli", "connection", "modify", "uuid", uuid};
    if (!r.password.empty()) {
        a.insert(a.end(), {"wifi-sec.key-mgmt", "wpa-psk", "wifi-sec.psk", r.password});
    } else if (uuid.empty()) {
        return a;  // a new open network
    } else {
        a.insert(a.end(), {"connection.autoconnect", "yes"});  // the password it has
    }
    return a;
}

Args upArgs(const std::string& uuid, int waitS) {
    return {"nmcli", "--wait", std::to_string(waitS), "connection", "up", "uuid", uuid};
}

Args apAutoconnectArgs(bool on) {
    return {"nmcli", "connection", "modify", kApProfile, "connection.autoconnect", on ? "yes" : "no"};
}

std::vector<std::string> splitTerse(const std::string& line) {
    std::vector<std::string> out(1);
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\\' && i + 1 < line.size()) out.back() += line[++i];
        else if (c == ':') out.emplace_back();
        else out.back() += c;
    }
    return out;
}

std::string field(const std::string& text, const std::string& name) {
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        if (text.compare(at, name.size(), name) == 0 && at + name.size() < text.size() && text[at + name.size()] == ':')
            return text.substr(at + name.size() + 1, end - at - name.size() - 1);
        at = end + 1;
    }
    return "";
}

std::vector<Seen> parseScan(const std::string& text) {
    std::vector<Seen> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::vector<std::string> f = splitTerse(text.substr(at, end - at));
        at = end + 1;
        if (f.size() < 3 || f[0].empty()) continue;
        Seen s{f[0], f[2], std::atoi(f[1].c_str())};
        auto same = std::find_if(out.begin(), out.end(), [&](const Seen& o) { return o.ssid == s.ssid; });
        if (same == out.end()) out.push_back(s);
        else if (s.signal > same->signal) *same = s;
    }
    std::stable_sort(out.begin(), out.end(), [](const Seen& a, const Seen& b) { return a.signal > b.signal; });
    return out;
}

}  // namespace aow::wifi
