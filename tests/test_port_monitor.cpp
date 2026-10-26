// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors

#include "aow/port_monitor.h"

#include <cstring>
#include <string>
#include <vector>

#include "check.h"

using namespace aow;

// An RTCM 3 frame of the given message number with a good CRC-24Q.
static std::vector<uint8_t> rtcm(uint16_t type, size_t payload) {
    std::vector<uint8_t> f = {0xD3, static_cast<uint8_t>(payload >> 8), static_cast<uint8_t>(payload & 0xFF),
                              static_cast<uint8_t>(type >> 4), static_cast<uint8_t>((type & 0x0F) << 4)};
    f.resize(3 + payload, 0x5A);
    uint32_t crc = 0;
    for (uint8_t b : f) {
        crc ^= static_cast<uint32_t>(b) << 16;
        for (int i = 0; i < 8; ++i) {
            crc <<= 1;
            if (crc & 0x1000000) crc ^= 0x1864CFB;
        }
    }
    f.push_back(static_cast<uint8_t>(crc >> 16));
    f.push_back(static_cast<uint8_t>(crc >> 8));
    f.push_back(static_cast<uint8_t>(crc));
    return f;
}

static void feed(PortMonitor& m, const std::string& s, uint64_t us) {
    m.feed(reinterpret_cast<const uint8_t*>(s.data()), s.size(), us);
}

int main() {
    // Lines: the first 20 characters each, CR LF or LF, the last four, oldest first.
    {
        PortMonitor m;
        const std::string a = "$GPGGA,123519,4807.038,N,01131.000,E,1,08*47\r\n$GPVTG,054.7,T";
        const std::string b = ",034.4,M*48\r\nOK\n\n\r\nA\x01" "B\r\nlast\n";
        feed(m, a, 1000);
        CHECK(m.lineCount() == 1 && !std::strcmp(m.line(0), "$GPGGA,123519,4807.0"));
        feed(m, b, 2000);  // empty lines are not lines
        CHECK(m.lineCount() == 4 && !std::strcmp(m.line(0), "$GPVTG,054.7,T,034.4"));  // the GGA has gone
        CHECK(!std::strcmp(m.line(1), "OK") && !std::strcmp(m.line(2), "A.B") && !std::strcmp(m.line(3), "last"));
        CHECK(!std::strcmp(m.line(4), ""));
        feed(m, "fifth\n", 3000);
        CHECK(m.lineCount() == 4 && !std::strcmp(m.line(0), "OK") && !std::strcmp(m.line(3), "fifth"));
        CHECK(m.bytes() == a.size() + b.size() + 6 && m.lastUs() == 3000);
    }

    // Binary with no line ends: a line every 200 bytes, so something shows.
    {
        PortMonitor m;
        const std::vector<uint8_t> bin(450, 0xAA);
        m.feed(bin.data(), bin.size(), 1);
        CHECK(m.lineCount() == 2 && !std::strcmp(m.line(0), "...................."));
    }

    // RTCM: message numbers with good CRCs, ascending, forgotten after 5 s; a bad CRC is not counted.
    {
        PortMonitor m;
        std::vector<uint8_t> all;
        for (uint16_t t : {1077, 1005, 1087, 1077}) {
            const auto f = rtcm(t, 20);
            all.insert(all.end(), f.begin(), f.end());
        }
        auto bad = rtcm(1230, 8);
        bad[6] ^= 1;
        all.insert(all.end(), bad.begin(), bad.end());
        for (size_t i = 0; i < all.size(); i += 7) m.feed(&all[i], std::min<size_t>(7, all.size() - i), 1'000'000);
        uint16_t types[PortMonitor::kRtcmTypes];
        int n = m.rtcmTypes(1'000'000, types, PortMonitor::kRtcmTypes);
        CHECK(n == 3 && types[0] == 1005 && types[1] == 1077 && types[2] == 1087);
        const auto late = rtcm(1230, 8);
        m.feed(late.data(), late.size(), 5'500'000);
        n = m.rtcmTypes(6'500'000, types, PortMonitor::kRtcmTypes);  // the first three are 5.5 s old
        CHECK(n == 1 && types[0] == 1230);
        CHECK(m.rtcmTypes(6'500'000, types, 0) == 0);
    }

    return finish("port_monitor");
}
