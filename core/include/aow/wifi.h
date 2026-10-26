// The CM4's Wi-Fi on the Network page: a client of the networks the unit knows (as set up by
// Raspberry Pi Imager, or added here), or an access point of its own for the cab, which the
// tablets join to reach AgOpenWeb and the AOW link gateway. NetworkManager does the work; this is
// the part with no Linux in it: what a request may say, and the nmcli commands that carry it out.
// The commands are argument lists, run without a shell, so an SSID or password is never parsed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aow::wifi {

constexpr const char* kDevice = "wlan0";
constexpr const char* kApProfile = "aow-ap";  // NetworkManager's name for the access point
constexpr const char* kApAddress = "10.42.0.1";  // the unit on its own network; tablets get 10.42.0.x
constexpr int kApPrefix = 24;
constexpr int kApPriority = 100;  // above any client network, so the access point wins at boot

enum class Mode : uint8_t { Client, AccessPoint };
inline const char* modeName(Mode m) { return m == Mode::AccessPoint ? "ap" : "client"; }

// One change from the page. In client mode the SSID is the network to join; an empty password
// keeps the one stored for it (or joins an open network). In access point mode the SSID and
// password are the unit's own; an empty password keeps the one stored.
struct Request {
    Mode mode = Mode::Client;
    std::string ssid, password;
    bool band5 = false;   // access point: 5 GHz (needs the country set); else 2.4 GHz
    uint8_t channel = 6;  // access point
};

// "" if the request can be carried out, else why not, for the page. passwordKnown: there is a
// stored password to keep; country is the regulatory domain ("00" or "": not set).
std::string requestError(const Request& r, bool passwordKnown, const std::string& country);

// The channels offered: 2.4 GHz 1-11 (allowed everywhere), 5 GHz 36-48 (no radar detection).
bool validChannel(bool band5, uint8_t channel);

// The default access point name for a unit with this hostname: "AgOpenWeb-<host>".
std::string defaultApSsid(const std::string& hostname);

// What the Network page shows.
struct State {
    bool available = false;  // NetworkManager and wlan0 are there
    std::string unavailable;  // else why not
    Mode mode = Mode::Client;  // the access point's autoconnect: what the unit does at boot
    bool fallback = true;  // client mode: the access point comes up if no known network is found after boot
    bool fallbackActive = false;  // it has
    // wlan0 now.
    bool connected = false, apActive = false;
    std::string ssid, ip;
    // The access point's profile.
    bool apExists = false, apBand5 = false;
    std::string apSsid;
    uint8_t apChannel = 6;
    std::vector<std::string> known;  // the client networks the unit knows, by SSID
    std::string country, hostname;
    // The last switch: "idle", "switching", "done" or "failed", and what happened.
    std::string job = "idle", message;
};

using Args = std::vector<std::string>;

// The access point's profile, made (exists false) or changed. Autoconnect is left off: it is
// switched on once the access point is up (apAutoconnect), so a failed switch leaves the unit as
// it was at the next boot too.
Args apProfileArgs(const Request& r, bool exists);
// A client network's profile: changed if the unit knows the network (uuid given), else made.
// Only a given password is written; the key management follows it.
Args clientProfileArgs(const Request& r, const std::string& uuid);
// Bring a profile up and wait for it, at most waitS seconds.
Args upArgs(const std::string& uuid, int waitS);
// The access point's autoconnect: on in access point mode, off in client mode.
Args apAutoconnectArgs(bool on);

// nmcli's terse output (-t): fields split at ':' with '\:' and '\\' as escapes.
std::vector<std::string> splitTerse(const std::string& line);
// One field of nmcli's terse multiline output ("NAME:value" lines, not escaped); "" if absent.
std::string field(const std::string& nmcliOutput, const std::string& name);

// The networks a scan saw, strongest first, each SSID once (hidden ones left out).
struct Seen { std::string ssid, security; int signal = 0; };
std::vector<Seen> parseScan(const std::string& nmciOutput);  // -t -f SSID,SIGNAL,SECURITY

}  // namespace aow::wifi
