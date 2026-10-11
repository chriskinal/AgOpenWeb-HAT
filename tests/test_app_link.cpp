#include "aow/app_link.h"

#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "aow/pgn.h"
#include "check.h"

using namespace aow;
using Bytes = std::vector<uint8_t>;

struct FakeUdp : hal::UdpLink {
    std::deque<Bytes> in;
    std::vector<Bytes> out;
    int receive(uint8_t* buf, size_t cap) override {
        if (in.empty()) return 0;
        Bytes b = in.front();
        in.pop_front();
        const size_t n = b.size() < cap ? b.size() : cap;
        std::memcpy(buf, b.data(), n);
        return static_cast<int>(n);
    }
    bool send(const uint8_t* buf, size_t len) override {
        out.emplace_back(buf, buf + len);
        return true;
    }
};

struct FakeClock : hal::Clock {
    uint64_t us = 1'000'000;
    uint64_t nowUs() override { return us; }
};

static Bytes frame(uint8_t src, uint8_t pgnId, const Bytes& data) {
    uint8_t buf[300];
    const size_t n = pgn::build(src, pgnId, data.data(), static_cast<uint8_t>(data.size()), buf, sizeof buf);
    return Bytes(buf, buf + n);
}

static bool parsed(const Bytes& b, pgn::Frame& f) { return pgn::parse(b.data(), b.size(), f); }

struct FakeSteer : SteerModule {
    int data = 0, config = 0, settings = 0;
    size_t lastLen = 0;
    pgn::SteerStatus st;
    void onSteerData(const pgn::SteerCommand&) override { ++data; }
    void onSteerConfig(const uint8_t*, size_t n) override { ++config; lastLen = n; }
    void onSteerSettings(const uint8_t*, size_t n) override { ++settings; lastLen = n; }
    pgn::SteerStatus steerStatus() override { return st; }
};

int main() {
    const uint8_t ip[4] = {127, 0, 0, 1};
    FakeUdp udp;
    FakeClock clock;
    AppLink link(udp, clock, ip);
    pgn::Frame f;

    // Hello (as the app sends it, fixed crc) -> steer and GPS hellos.
    udp.in.push_back({0x80, 0x81, 0x7F, 0xC8, 0x03, 0x38, 0x00, 0x00, 0x47});
    link.poll();
    CHECK(udp.out.size() == 2);
    CHECK(parsed(udp.out[0], f) && f.src == pgn::SrcSteer && f.pgn == pgn::SrcSteer && f.len == 5);
    CHECK(parsed(udp.out[1], f) && f.src == pgn::SrcGps && f.pgn == pgn::SrcGps && f.len == 5);
    udp.out.clear();

    // Scan -> two PGN 203s carrying the module IP and subnet.
    udp.in.push_back({0x80, 0x81, 0x7F, 0xCA, 0x03, 0xCA, 0xCA, 0x05, 0x47});
    link.poll();
    CHECK(udp.out.size() == 2);
    CHECK(parsed(udp.out[0], f) && f.src == pgn::SrcSteer && f.pgn == pgn::ScanReply && f.len == 7);
    CHECK(std::memcmp(f.data, "\x7F\x00\x00\x01\x7F\x00\x00", 7) == 0);
    udp.out.clear();

    // No PGN 254 yet.
    CHECK(!link.haveCommand());
    CHECK(link.usSinceCommand() == UINT64_MAX);

    // PGN 254: 12.3 km/h, guidance + autosteer, target -4.56 deg, xte -2, sections 0x0201.
    const int16_t target = -456;
    const Bytes d254 = {123, 0, 0x41, static_cast<uint8_t>(target & 0xFF),
                        static_cast<uint8_t>((target >> 8) & 0xFF), 0xFE, 0x01, 0x02};
    udp.in.push_back(frame(pgn::kSrcHost, pgn::SteerData, d254));
    link.poll();
    CHECK(link.haveCommand());
    const pgn::SteerCommand& sd = link.command();
    CHECK(sd.speedKmh > 12.29f && sd.speedKmh < 12.31f);
    CHECK(sd.guidanceActive() && sd.autosteerEnabled());
    CHECK(sd.targetAngleDeg > -4.561f && sd.targetAngleDeg < -4.559f);
    CHECK(sd.xte == -2 && sd.sections == 0x0201);

    // ... answered by exactly one PGN 253, reporting the default status: all switches off.
    CHECK(udp.out.size() == 1);
    CHECK(parsed(udp.out[0], f) && f.src == pgn::SrcSteer && f.pgn == pgn::FromSteer && f.len == 8);
    CHECK(f.data[6] == 0x03 && f.data[7] == 0);
    udp.out.clear();

    // The 253 reports what setStatus says.
    pgn::SteerStatus st;
    st.actualAngleDeg = 1.5f;
    st.workSwitchOn = true;
    st.steerArmed = true;
    st.pwmDisplay = 200;
    link.setStatus(st);
    udp.in.push_back(frame(pgn::kSrcHost, pgn::SteerData, d254));
    link.poll();
    CHECK(udp.out.size() == 1);
    CHECK(parsed(udp.out[0], f) && f.data[0] == 150 && f.data[1] == 0);
    CHECK(f.data[6] == 0x00 && f.data[7] == 200);
    udp.out.clear();

    // Age of the last PGN 254 follows the clock.
    clock.us += 250'000;
    CHECK(link.usSinceCommand() == 250'000);

    // Rejected without an answer: a short PGN 254, a bad crc, a frame from another module.
    udp.in.push_back(frame(pgn::kSrcHost, pgn::SteerData, {1, 2, 3}));
    Bytes bad = frame(pgn::kSrcHost, pgn::SteerData, d254);
    bad.back() ^= 0xFF;
    udp.in.push_back(bad);
    udp.in.push_back(frame(pgn::SrcSteer, pgn::SteerData, d254));
    link.poll();
    CHECK(udp.out.empty());
    CHECK(link.usSinceCommand() == 250'000);  // none of them counted as fresh steer data
    CHECK(link.counters().rejected == 3);
    CHECK(link.counters().commands == 2);

    // With a steer module: 251/252/254 go to it, and PGN 253 reports its status.
    {
        FakeSteer steer;
        steer.st.steerArmed = true;
        steer.st.pwmDisplay = 77;
        link.setSteerModule(&steer);
        udp.out.clear();
        udp.in.push_back(frame(pgn::kSrcHost, pgn::SteerConfig, {1, 2, 3, 4, 5, 6, 7, 8}));
        udp.in.push_back(frame(pgn::kSrcHost, pgn::SteerSettings, {1, 2, 3, 4, 5, 6, 7, 8}));
        udp.in.push_back(frame(pgn::kSrcHost, pgn::SteerData, d254));
        link.poll();
        CHECK(steer.config == 1 && steer.settings == 1 && steer.data == 1 && steer.lastLen == 8);
        CHECK(udp.out.size() == 1 && parsed(udp.out[0], f) && f.pgn == pgn::FromSteer);
        CHECK(f.data[6] == 0x01 && f.data[7] == 77);  // armed (bit 1 clear), work off (bit 0 set)
        link.setSteerModule(nullptr);
    }

    // PGN 250.
    udp.out.clear();
    link.sendTurnSensor(123);
    CHECK(udp.out.size() == 1 && parsed(udp.out[0], f) && f.src == pgn::SrcSteer && f.pgn == pgn::TurnSensor);
    CHECK(f.len == 8 && f.data[0] == 123 && f.data[1] == 0);

    // PGN 221 popup; long messages are cut at 110 characters.
    udp.out.clear();
    link.sendPopup("Fendt steering valve not ready!", 5, 1);
    CHECK(udp.out.size() == 1 && parsed(udp.out[0], f) && f.pgn == pgn::HardwareMessage);
    CHECK(f.len == 2 + 31 && f.data[0] == 5 && f.data[1] == 1 && std::memcmp(f.data + 2, "Fendt", 5) == 0);
    link.sendPopup(std::string(200, 'x').c_str(), 3, 2);
    CHECK(parsed(udp.out[1], f) && f.len == 112);

    // Sections: the 16 in PGN 254 until PGN 229 brings all 64; back to 254 a second after the
    // last 229. PGN 241 is the app's ISOBUS section control switch, on until an app says off.
    {
        AppLink l(udp, clock, ip);
        CHECK(l.sections() == 0 && l.sectionControl());
        udp.in.push_back(frame(pgn::kSrcHost, pgn::SteerData, {50, 0, 1, 0, 0, 0, 0x05, 0x80}));
        l.poll();
        CHECK(l.sections() == 0x8005);
        udp.in.push_back(frame(pgn::kSrcHost, pgn::Sections64, {0x01, 0, 0, 0, 0, 0, 0, 0x80, 50, 50}));
        l.poll();
        CHECK(l.sections() == 0x8000000000000001ull);
        clock.us += 999'000;
        CHECK(l.sections() == 0x8000000000000001ull);
        clock.us += 2'000;
        CHECK(l.sections() == 0x8005);
        udp.in.push_back(frame(pgn::kSrcHost, pgn::Sections64, {1, 2, 3}));  // short: ignored
        udp.in.push_back(frame(pgn::kSrcHost, pgn::SectionControl, {0}));
        l.poll();
        CHECK(l.sections() == 0x8005 && !l.sectionControl());
        udp.in.push_back(frame(pgn::kSrcHost, pgn::SectionControl, {1}));
        l.poll();
        CHECK(l.sectionControl());
    }

    return finish("app_link");
}
