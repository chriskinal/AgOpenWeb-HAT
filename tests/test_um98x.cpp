// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors

#include "aow/um98x.h"

#include <deque>
#include <map>
#include <string>
#include <vector>

#include "check.h"

using namespace aow;

struct FakeClock : hal::Clock {
    uint64_t us = 1'000'000;
    uint64_t nowUs() override { return us; }
};

// A UM982 that answers each command line with scripted lines.
struct FakeReceiver : hal::SerialPort {
    std::map<std::string, std::vector<std::string>> script;
    std::vector<std::string> received;  // commands, in order
    std::deque<uint8_t> rx;
    std::string partial;
    bool silent = false;

    int read(uint8_t* buf, size_t cap) override {
        size_t n = 0;
        while (n < cap && !rx.empty()) { buf[n++] = rx.front(); rx.pop_front(); }
        return static_cast<int>(n);
    }
    int write(const uint8_t* buf, size_t len) override {
        partial.append(reinterpret_cast<const char*>(buf), len);
        size_t eol;
        while ((eol = partial.find("\r\n")) != std::string::npos) {
            const std::string cmd = partial.substr(0, eol);
            partial.erase(0, eol + 2);
            received.push_back(cmd);
            if (silent) continue;
            std::vector<std::string> reply = {"$command," + cmd + ",response: OK*00"};
            auto it = script.find(cmd);
            if (it != script.end()) reply.insert(reply.end(), it->second.begin(), it->second.end());
            for (const std::string& l : reply)
                for (char c : l + "\r\n") rx.push_back(static_cast<uint8_t>(c));
        }
        return static_cast<int>(len);
    }
};

int main() {
    // Parsers on their own.
    std::string out;
    CHECK(Um98x::parseModeResponse("junk\n#MODE,97,GPS,FINE,2190,1,0,0;MODE ROVER SURVEY,*1a\n", out) &&
          out == "MODE ROVER SURVEY");
    CHECK(Um98x::parseConfigResponse("$CONFIG,COM1,CONFIG COM1 460800*33\n$CONFIG,ANTENNA,CONFIG ANTENNA POWERON*7a\n", out) &&
          out == "CONFIG COM1 460800\nCONFIG ANTENNA POWERON");
    CHECK(Um98x::parseLogListResponse("<\t2\n<\tGPGGA COM1 0.1\n<\tGPHPR COM1 0.1\n", out) &&
          out == "GPGGA COM1 0.1\nGPHPR COM1 0.1");

    FakeClock clock;
    auto sleep = [&clock](uint32_t ms) { clock.us += ms * 1000ull; };

    FakeReceiver rx;
    rx.script["CONFIG"] = {"$CONFIG,ANTENNA,CONFIG ANTENNA POWERON*7a", "$CONFIG,COM1,CONFIG COM1 460800*33",
                           "$CONFIG,COM2,CONFIG COM2 115200*30", "$CONFIG,COM3,CONFIG COM3 115200*31"};
    rx.script["MODE"] = {"#MODE,97,GPS,FINE,2190,1,0,0;MODE ROVER SURVEY,*1a"};
    rx.script["UNILOGLIST"] = {"<\t2", "<\tGPGGA COM1 0.1", "<\tGPHPR COM1 0.1"};

    // Read: CONFIG, MODE and the logs, and the logs are switched back on afterwards.
    {
        Um98x um(rx, clock, sleep);
        Um98x::Config c;
        CHECK(um.readConfiguration(c));
        CHECK(c.configCommands == "CONFIG ANTENNA POWERON\nCONFIG COM1 460800\nCONFIG COM2 115200\nCONFIG COM3 115200");
        CHECK(c.modeSettings == "MODE ROVER SURVEY");
        CHECK(c.messageSettings == "GPGGA COM1 0.1\nGPHPR COM1 0.1");
        const auto& r = rx.received;
        CHECK(r.size() >= 2 && r[r.size() - 2] == "GPGGA COM1 0.1" && r.back() == "GPHPR COM1 0.1");
    }

    // Write: logs off, CONFIG lines, MODE, logs off, the new logs, SAVECONFIG last.
    {
        rx.received.clear();
        Um98x um(rx, clock, sleep);
        Um98x::Config c;
        c.configCommands = "CONFIG COM1 460800\n\n  CONFIG HEADING FIXLENGTH  ";
        c.modeSettings = "MODE ROVER SURVEY";
        c.messageSettings = "GPGGA COM1 0.1\nGPHPR COM1 0.1";
        CHECK(um.writeConfiguration(c));
        const std::vector<std::string> want = {"UNLOGALL COM1", "UNLOGALL COM2", "UNLOGALL COM3",
                                               "CONFIG COM1 460800", "CONFIG HEADING FIXLENGTH", "MODE ROVER SURVEY",
                                               "UNLOGALL COM1", "UNLOGALL COM2", "UNLOGALL COM3",
                                               "GPGGA COM1 0.1", "GPHPR COM1 0.1", "SAVECONFIG"};
        CHECK(rx.received == want);
    }

    // A quiet adjustment: logs off, CONFIG to `decide`, its commands, the logs back on, then
    // SAVECONFIG (so the saved settings keep the logs).
    {
        rx.received.clear();
        Um98x um(rx, clock, sleep);
        std::string seen;
        CHECK(um.adjustConfiguration([&seen](const std::string& config) {
            seen = config;
            return std::vector<std::string>{"CONFIG INSSOL OFFSET 0.10 0.20 0.30"};
        }));
        CHECK(seen == "CONFIG ANTENNA POWERON\nCONFIG COM1 460800\nCONFIG COM2 115200\nCONFIG COM3 115200");
        const std::vector<std::string> want = {"UNILOGLIST", "UNLOGALL COM1", "UNLOGALL COM2", "UNLOGALL COM3",
                                               "CONFIG", "CONFIG INSSOL OFFSET 0.10 0.20 0.30",
                                               "GPGGA COM1 0.1", "GPHPR COM1 0.1", "SAVECONFIG"};
        CHECK(rx.received == want);
        // Nothing to change: no SAVECONFIG, the logs still come back.
        rx.received.clear();
        CHECK(um.adjustConfiguration([](const std::string&) { return std::vector<std::string>{}; }));
        CHECK(rx.received.size() == 7 && rx.received.back() == "GPHPR COM1 0.1");
    }

    // A receiver that never answers: read and write fail (after their timeouts), no hang.
    {
        rx.silent = true;
        rx.received.clear();
        Um98x um(rx, clock, sleep);
        Um98x::Config c;
        CHECK(!um.readConfiguration(c));
        c.configCommands = "CONFIG COM1 460800";
        CHECK(!um.writeConfiguration(c));
        CHECK(rx.received.back() == "CONFIG COM1 460800");  // stopped at the first failure
        // Adjusting: it gives up at UNILOGLIST, before stopping the logs it could not put back.
        rx.received.clear();
        bool asked = false;
        CHECK(!um.adjustConfiguration([&asked](const std::string&) { asked = true; return std::vector<std::string>{}; }));
        CHECK(!asked && rx.received.size() == 1 && rx.received[0] == "UNILOGLIST");
    }

    return finish("um98x");
}
