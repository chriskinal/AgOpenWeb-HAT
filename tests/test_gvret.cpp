// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors

#include "aow/gvret.h"

#include <string>

#include "check.h"

using namespace aow::gvret;

static std::string in(Protocol& p, std::string bytes, uint32_t now = 0x11223344) {
    return p.input(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), now);
}

int main() {
    Protocol p;
    CHECK(!p.binaryMode());
    CHECK(in(p, "\xE7").empty() && p.binaryMode());

    // SavvyCAN's connect sequence: device info, bus count, bus params, keepalive, time sync.
    CHECK(in(p, "\xF1\x07") == std::string("\xF1\x07\x84\x17\x20\x00\x07", 7));  // build 6020 = 0x1784
    CHECK(in(p, "\xF1\x0C") == std::string("\xF1\x0C\x03", 3));
    const std::string params = in(p, "\xF1\x06");
    CHECK(params.size() == 17 && params.substr(2, 5) == std::string("\x01\x20\xA1\x07\x00", 5));  // 500000
    CHECK(in(p, "\xF1\x09") == std::string("\xF1\x09\xDE\xAD", 4));
    CHECK(in(p, "\xF1\x01") == std::string("\xF1\x01\x44\x33\x22\x11", 6));

    // Several commands in one read, split across reads, and a frame from SavvyCAN (dropped).
    CHECK(in(p, std::string("\xF1\x09\xF1", 3)) == std::string("\xF1\x09\xDE\xAD", 4));
    CHECK(in(p, "\x0C") == std::string("\xF1\x0C\x03", 3));
    CHECK(in(p, std::string("\xF1\x00\x21\x06\x00\x00\x02\xAA\xBB\x00", 10)).empty());
    CHECK(in(p, "\xF1\x09").size() == 4);  // back in step after the frame

    // A mirrored frame: extended id, bus 2, 3 bytes.
    std::string f;
    const uint8_t data[] = {1, 2, 3};
    encodeFrame(f, 2, 0x0CAD131C, true, data, 3, 0x01020304);
    CHECK(f == std::string("\xF1\x00\x04\x03\x02\x01\x1C\x13\xAD\x8C\x23\x01\x02\x03\x00", 15));
    f.clear();
    encodeFrame(f, 0, 0x123, false, data, 1, 0);
    CHECK(f.size() == 13 && static_cast<uint8_t>(f[9]) == 0x00 && f[10] == 0x01);

    return finish("gvret");
}
