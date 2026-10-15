// External IMU: BNO085 RVC and TM171 packets, detection, the yaw rate, and the IMU fields in
// PANDA and PAOGI.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "aow/gnss.h"
#include "aow/imu.h"
#include "aow/nav.h"
#include "aow/nmea.h"
#include "check.h"

using namespace aow;
using Bytes = std::vector<uint8_t>;

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static void put16(Bytes& b, int v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

// One BNO085 RVC packet: angles in degrees, sent in 0.01 deg.
static Bytes bno(uint8_t index, double yaw, double pitch, double roll) {
    Bytes b = {0xAA, 0xAA, index};
    put16(b, static_cast<int>(std::lround(yaw * 100)));
    put16(b, static_cast<int>(std::lround(pitch * 100)));
    put16(b, static_cast<int>(std::lround(roll * 100)));
    for (int i = 0; i < 9; ++i) b.push_back(0);  // accelerations and reserved
    uint8_t sum = 0;
    for (size_t i = 2; i < b.size(); ++i) sum = static_cast<uint8_t>(sum + b[i]);
    b.push_back(sum);
    return b;
}

static uint16_t crc16(const uint8_t* d, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (int j = 0; j < 8; ++j) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

static void putFloat(Bytes& b, float f) {
    uint8_t x[4];
    std::memcpy(x, &f, 4);
    b.insert(b.end(), x, x + 4);
}

// One TM171 packet: object id and payload after the 4 info bytes.
static Bytes tm171(uint8_t object, const Bytes& body) {
    Bytes b = {0xAA, 0x55, static_cast<uint8_t>(4 + body.size()), object, 0, 0, 0};
    b.insert(b.end(), body.begin(), body.end());
    const uint16_t crc = crc16(&b[2], b.size() - 2);
    b.push_back(static_cast<uint8_t>(crc & 0xFF));
    b.push_back(static_cast<uint8_t>(crc >> 8));
    return b;
}

static Bytes tmRpy(float roll, float pitch, float yaw) {
    Bytes body;
    putFloat(body, 0);  // timestamp
    putFloat(body, roll);
    putFloat(body, pitch);
    putFloat(body, yaw);
    return tm171(0x23, body);
}

static void feed(imu::Imu& m, const Bytes& b, uint64_t us) { m.feed(b.data(), b.size(), us); }

static std::string sentence(const char* out, size_t n) { return std::string(out, n); }

int main() {
    // --- BNO085: three packets to detect, then angles, heading in 0..360
    {
        imu::Imu m;
        feed(m, bno(0, -90.0, 2.5, -1.25), 0);
        feed(m, bno(1, -90.0, 2.5, -1.25), 10'000);
        CHECK(m.type() == imu::Type::None && !m.valid(10'000));
        feed(m, bno(2, -90.0, 2.5, -1.25), 20'000);
        CHECK(m.type() == imu::Type::Bno085 && m.valid(20'000) && m.packets() == 1);
        CHECK(near(m.data().heading, 270.0, 1e-3));
        CHECK(near(m.data().pitch, 2.5, 1e-3) && near(m.data().roll, -1.25, 1e-3));
        CHECK(!m.valid(20'000 + 100'000));  // quiet for 100 ms: not valid, as the firmware

        // A bad checksum is dropped.
        Bytes bad = bno(3, 10.0, 0, 0);
        bad.back() ^= 0xFF;
        feed(m, bad, 30'000);
        CHECK(m.packets() == 1);

        // Split across reads, with noise before it.
        Bytes p = bno(4, 45.0, 0, 0);
        const Bytes noise = {0x00, 0xAA, 0x13, 0x55};
        feed(m, noise, 40'000);
        feed(m, Bytes(p.begin(), p.begin() + 7), 40'000);
        feed(m, Bytes(p.begin() + 7, p.end()), 40'000);
        CHECK(m.packets() == 2 && near(m.data().heading, 45.0, 1e-3));
    }

    // --- Yaw rate: a real rate in deg/s, across the +-180 wrap, zero when parked
    {
        imu::Imu m;
        // Turning at +12 deg/s (heading increasing) through 180: 100 Hz packets for 1 s.
        double yaw = 175.0;
        for (int i = 0; i < 100; ++i) {
            double sent = yaw > 180.0 ? yaw - 360.0 : yaw;
            feed(m, bno(static_cast<uint8_t>(i), sent, 0, 0), static_cast<uint64_t>(i) * 10'000);
            yaw += 0.12;
        }
        CHECK(near(m.data().yawRate, 12.0, 0.05));
        CHECK(m.data().heading > 180.0f && m.data().heading < 190.0f);

        // Parked: same yaw for half a second -> 0, not the old "whole angle" spike.
        const uint64_t t = 99 * 10'000;
        for (int i = 1; i <= 50; ++i) feed(m, bno(static_cast<uint8_t>(i), -173.12, 0, 0), t + i * 10'000);
        CHECK(near(m.data().yawRate, 0.0, 1e-3));

        // Turning the other way across the wrap: -20 deg/s.
        imu::Imu n;
        yaw = -175.0;
        for (int i = 0; i < 60; ++i) {
            double sent = yaw < -180.0 ? yaw + 360.0 : yaw;
            feed(n, bno(static_cast<uint8_t>(i), sent, 0, 0), static_cast<uint64_t>(i) * 10'000);
            yaw -= 0.2;
        }
        CHECK(near(n.data().yawRate, -20.0, 0.05));

        // After a gap of more than half a second the rate starts again from nothing.
        feed(n, bno(0, 90.0, 0, 0), 60 * 10'000 + 2'000'000);
        CHECK(near(n.data().yawRate, 0.0, 1e-6));
    }

    // --- TM171: roll as sent (the firmware negates it), other objects skipped, bad CRC counted
    {
        imu::Imu m;
        for (int i = 0; i < 3; ++i) feed(m, tmRpy(1.5f, -0.5f, -30.0f), static_cast<uint64_t>(i) * 10'000);
        CHECK(m.type() == imu::Type::Tm171);
        CHECK(near(m.data().roll, 1.5, 1e-5) && near(m.data().pitch, -0.5, 1e-5));
        CHECK(near(m.data().heading, 330.0, 1e-4));
        const uint32_t before = m.packets();
        feed(m, tm171(0x10, Bytes(12, 0)), 30'000);  // another object: no sample
        CHECK(m.packets() == before);
        Bytes bad = tmRpy(0, 0, 0);
        bad[10] ^= 0x01;
        feed(m, bad, 40'000);
        CHECK(m.packets() == before);

        // Once a TM171, BNO packets are ignored.
        for (int i = 0; i < 3; ++i) feed(m, bno(static_cast<uint8_t>(i), 10, 0, 0), 50'000);
        CHECK(m.type() == imu::Type::Tm171 && m.packets() == before);

        // Its yaw rate is measured too (the firmware's is always 0).
        float yaw = 0;
        for (int i = 0; i < 30; ++i, yaw += 0.05f) feed(m, tmRpy(0, 0, yaw), 100'000 + static_cast<uint64_t>(i) * 10'000);
        CHECK(near(m.data().yawRate, 5.0, 0.05));
    }

    // --- Y-axis swap (PGN 251): roll and pitch trade places
    {
        imu::Imu m;
        m.setSwapXY(true);
        for (int i = 0; i < 3; ++i) feed(m, bno(static_cast<uint8_t>(i), 0, 3.0, -2.0), static_cast<uint64_t>(i) * 10'000);
        CHECK(near(m.data().roll, 3.0, 1e-3) && near(m.data().pitch, -2.0, 1e-3));
    }

    // --- PANDA and PAOGI carry the IMU: heading x10, roll x10, pitch in degrees, yaw rate deg/s
    {
        imu::Data d;
        d.heading = 271.37f;
        d.roll = -1.26f;
        d.pitch = 3.4f;
        d.yawRate = 5.678f;

        nmea::SentenceReader r;
        gnss::Parser p;
        const std::string gga = "$GNGGA,123519.00,4807.038,N,01131.000,E,4,12,0.9,545.4,M,46.9,M,1.0,0000";
        char cs[8];
        std::snprintf(cs, sizeof cs, "*%02X\r\n", nmea::xorChecksum(gga.data() + 1, gga.size() - 1));
        for (char c : gga + cs)
            if (r.feed(c)) p.parse(r.sentence(), 1);
        char out[256];
        const std::string panda = sentence(out, nav::buildPanda(p.state(), &d, out, sizeof out));
        CHECK(panda.find(",2713,-13,3,5.68*") != std::string::npos);
        const std::string none = sentence(out, nav::buildPanda(p.state(), nullptr, out, sizeof out));
        CHECK(none.find(",65535,0,0,0*") != std::string::npos);

        const std::string hpr = "$GNHPR,123519.00,90.00,-1.50,0.30,4,24,1.0,0000";
        std::snprintf(cs, sizeof cs, "*%02X\r\n", nmea::xorChecksum(hpr.data() + 1, hpr.size() - 1));
        for (char c : hpr + cs)
            if (r.feed(c)) p.parse(r.sentence(), 1);
        const std::string paogi = sentence(out, nav::build(p.state(), &d, 1, out, sizeof out));
        CHECK(paogi.find(",90.0,-1.50,3,5.68*") != std::string::npos);  // dual heading and roll; IMU pitch and rate
    }

    return finish("imu");
}
