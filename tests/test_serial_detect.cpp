#include "aow/serial_detect.h"

#include <cstdint>
#include <string>
#include <vector>

#include "check.h"

using namespace aow;
using K = SerialDetector::Kind;

static std::vector<uint8_t> rtcmFrame(uint16_t type) {
    std::vector<uint8_t> f = {0xD3, 0x00, 0x08, static_cast<uint8_t>(type >> 4), static_cast<uint8_t>((type & 0x0F) << 4)};
    f.resize(3 + 8, 0x11);
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

static std::vector<uint8_t> bno(uint8_t index) {  // a level, still BNO085 RVC packet
    std::vector<uint8_t> p = {0xAA, 0xAA, index};
    p.resize(18, 0);
    uint8_t sum = 0;
    for (int i = 2; i < 18; ++i) sum = static_cast<uint8_t>(sum + p[i]);
    p.push_back(sum);
    return p;
}

static void feed(SerialDetector& d, const std::string& s, uint64_t us) {
    d.feed(reinterpret_cast<const uint8_t*>(s.data()), s.size(), us);
}
static void feed(SerialDetector& d, const std::vector<uint8_t>& v, uint64_t us) { d.feed(v.data(), v.size(), us); }

int main() {
    const std::string gga = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";

    // GNSS: two good sentences. Kept while they come; lost after 5 s without; listened for again.
    {
        SerialDetector d(0, 0);
        CHECK(d.kind() == K::Unknown && d.baud() == 115200 && d.autoBaud());
        feed(d, gga, 100'000);
        CHECK(d.kind() == K::Unknown);
        feed(d, gga, 200'000);
        CHECK(d.kind() == K::Gnss && d.lastValidUs() == 200'000);
        CHECK(!d.tick(5'000'000) && d.kind() == K::Gnss);
        feed(d, "garbage\r\n", 4'000'000);  // not a sentence: does not keep it alive
        CHECK(!d.tick(5'300'000) && d.kind() == K::Unknown && d.baud() == 115200);  // same rate first
        feed(d, gga + gga, 5'400'000);
        CHECK(d.kind() == K::Gnss);
    }

    // A base receiver (NMEA and RTCM): GNSS.
    {
        SerialDetector d(0, 0);
        feed(d, rtcmFrame(1005), 10);
        feed(d, gga + gga, 20);
        CHECK(d.kind() == K::Gnss);  // the RTCM came first, but radios send no NMEA
        SerialDetector base(0, 0);
        std::vector<uint8_t> mix(gga.begin(), gga.end());
        const auto f = rtcmFrame(1077);
        mix.insert(mix.end(), f.begin(), f.end());
        mix.insert(mix.end(), gga.begin(), gga.end());
        feed(base, mix, 10);
        CHECK(base.kind() == K::Gnss);
    }

    // Radio: one good RTCM frame; a bad CRC is nothing. Kept by more frames.
    {
        SerialDetector d(0, 0);
        auto bad = rtcmFrame(1077);
        bad[5] ^= 0xFF;
        feed(d, bad, 10);
        CHECK(d.kind() == K::Unknown);
        feed(d, rtcmFrame(1077), 1'000'000);
        CHECK(d.kind() == K::Radio);
        feed(d, rtcmFrame(1087), 4'000'000);
        CHECK(!d.tick(8'000'000) && d.kind() == K::Radio);
    }

    // IMU: three BNO packets.
    {
        SerialDetector d(0, 0);
        for (uint8_t i = 0; i < 3; ++i) feed(d, bno(i), 10'000u * i);
        CHECK(d.kind() == K::Imu);
    }

    // Auto rate: a new rate every 2.5 s with nothing recognised, round the list; pinned: never.
    {
        SerialDetector d(0, 0);
        feed(d, "\xF0\x0F\xF0 noise\r\n", 100);
        CHECK(!d.tick(2'000'000) && d.baud() == 115200);
        CHECK(d.tick(2'600'000) && d.baud() == 460800);
        CHECK(!d.tick(4'000'000));
        CHECK(d.tick(5'200'000) && d.baud() == 38400);
        uint64_t t = 5'200'000;
        for (int i = 0; i < 5; ++i) CHECK(d.tick(t += 2'600'000));
        CHECK(d.baud() == 115200);  // round again
        SerialDetector p(9600, 0);
        CHECK(!p.autoBaud() && p.baud() == 9600 && !p.tick(60'000'000) && p.baud() == 9600);
        p.reset(0, 60'000'000);
        CHECK(p.autoBaud() && p.baud() == 115200);
    }

    // Found at a later rate: stays there.
    {
        SerialDetector d(0, 0);
        CHECK(d.tick(2'600'000) && d.baud() == 460800);
        feed(d, gga + gga, 3'000'000);
        CHECK(d.kind() == K::Gnss && !d.tick(5'700'000) && d.baud() == 460800);
    }

    return finish("serial_detect");
}
