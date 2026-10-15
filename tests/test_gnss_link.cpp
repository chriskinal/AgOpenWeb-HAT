#include "aow/gnss_link.h"

#include <cstring>
#include <string>
#include <vector>

#include "check.h"

using namespace aow;

struct FakeSerial : hal::SerialPort {
    std::string rx;           // bytes the receiver has sent
    std::vector<uint8_t> tx;  // bytes written to the receiver
    size_t chunk = 7;         // deliver in small pieces to exercise reassembly
    int writeLimit = -1;      // -1: take everything
    int read(uint8_t* buf, size_t cap) override {
        size_t n = rx.size() < chunk ? rx.size() : chunk;
        if (n > cap) n = cap;
        std::memcpy(buf, rx.data(), n);
        rx.erase(0, n);
        return static_cast<int>(n);
    }
    int write(const uint8_t* buf, size_t len) override {
        const size_t n = writeLimit < 0 ? len : std::min(len, static_cast<size_t>(writeLimit));
        tx.insert(tx.end(), buf, buf + n);
        return static_cast<int>(n);
    }
};

struct FakeUdp : hal::UdpLink {
    std::vector<std::string> out;
    int receive(uint8_t*, size_t) override { return 0; }
    bool send(const uint8_t* buf, size_t len) override {
        out.emplace_back(reinterpret_cast<const char*>(buf), len);
        return true;
    }
};

struct FakeClock : hal::Clock {
    uint64_t us = 1'000'000;
    uint64_t nowUs() override { return us; }
};

int main() {
    FakeSerial serial;
    FakeUdp app;
    FakeClock clock;
    GnssLink link(serial, app, clock);

    // A GGA gives one PANDA; the VTG before it gives none of its own.
    serial.rx = "$GPVTG,054.7,T,034.4,M,005.5,N,010.2,K*48\r\n"
                "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
    link.pollReceiver();
    CHECK(link.counters().sentences == 2);
    CHECK(app.out.size() == 1);
    CHECK(app.out.size() == 1 && app.out[0].rfind("$PANDA,123519.0,4807.038000,N,", 0) == 0);
    CHECK(app.out.size() == 1 && app.out[0].find(",5.500,65535,0,0,0*") != std::string::npos);
    CHECK(link.state().fixQuality == 1);

    // No fix: GGA parsed, nothing sent.
    app.out.clear();
    serial.rx = "$GPGGA,123520,,,,,0,00,,,M,,M,,*61\r\n";
    link.pollReceiver();
    CHECK(app.out.empty());

    // A corrupted sentence is counted, not applied.
    serial.rx = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48\r\n";
    link.pollReceiver();
    CHECK(app.out.empty() && link.reader().badChecksums() == 1);

    // RTCM goes to the receiver byte for byte; runts are ignored.
    const uint8_t rtcm[] = {0xD3, 0x00, 0x13, 0x3E, 0xD7, 0xD3, 0x02, 0x02};
    link.forwardRtcm(rtcm, sizeof rtcm);
    CHECK(serial.tx.size() == sizeof rtcm && std::memcmp(serial.tx.data(), rtcm, sizeof rtcm) == 0);
    link.forwardRtcm(rtcm, 4);
    CHECK(serial.tx.size() == sizeof rtcm && link.counters().rtcmPackets == 1);

    // A full serial buffer: the shortfall is counted.
    serial.writeLimit = 3;
    link.forwardRtcm(rtcm, sizeof rtcm);
    CHECK(link.counters().rtcmBytesDropped == sizeof rtcm - 3);

    // Passthrough: sentences go to the app as received (VTG too), not PANDA.
    app.out.clear();
    link.setPassThrough(true);
    serial.rx = "$GPVTG,054.7,T,034.4,M,005.5,N,010.2,K*48\r\n"
                "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
    link.pollReceiver();
    CHECK(app.out.size() == 2);
    CHECK(app.out.size() == 2 && app.out[0] == "$GPVTG,054.7,T,034.4,M,005.5,N,010.2,K*48\r\n");
    CHECK(app.out.size() == 2 && app.out[1].rfind("$GPGGA,123519,", 0) == 0 && app.out[1].find("*47\r\n") != std::string::npos);
    link.setPassThrough(false);

    // Radio RTCM goes straight through, however it is chunked.
    serial.tx.clear();
    serial.writeLimit = -1;
    link.forwardRadio(rtcm, 3);
    CHECK(serial.tx.size() == 3 && link.counters().radioBytes == 3);

    // RTCM out: a copy of what the receiver is sent, from the app and the radio.
    FakeSerial rtcmOut;
    link.setRtcmOut(&rtcmOut);
    link.forwardRtcm(rtcm, sizeof rtcm);
    link.forwardRadio(rtcm, 3);
    link.forwardRtcm(rtcm, 4);  // a runt: not to the receiver, so not out either
    CHECK(rtcmOut.tx.size() == sizeof rtcm + 3 && std::memcmp(rtcmOut.tx.data(), rtcm, sizeof rtcm) == 0);
    CHECK(link.counters().rtcmOutBytes == sizeof rtcm + 3);
    rtcmOut.writeLimit = 2;
    link.forwardRadio(rtcm, 3);
    CHECK(link.counters().outBytesDropped == 1 && serial.tx.size() == 3 + sizeof rtcm + 3 + 3);
    link.setRtcmOut(nullptr);

    // NMEA out: only the chosen types, as received, any talker; PANDA to the app as before.
    const std::string vtg = "$GPVTG,054.7,T,034.4,M,005.5,N,010.2,K*48\r\n";
    const std::string gga = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
    const std::string rmc = "$GNRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*74\r\n";
    FakeSerial nmeaOut;
    link.setNmeaOut(&nmeaOut, 1 | 4, 0);  // GGA and RMC, every one
    app.out.clear();
    serial.rx = vtg + gga + rmc;
    link.pollReceiver();
    const std::string sent(nmeaOut.tx.begin(), nmeaOut.tx.end());
    CHECK(sent == gga + rmc);
    CHECK(link.counters().nmeaOutSent == 2 && app.out.size() == 1 && app.out[0].rfind("$PANDA", 0) == 0);

    // At 5 Hz from a 10 Hz receiver: every other GGA.
    nmeaOut.tx.clear();
    link.setNmeaOut(&nmeaOut, 1, 5);
    for (int i = 0; i < 10; ++i) {
        clock.us += 100'000;
        serial.rx = gga;
        link.pollReceiver();
    }
    CHECK(nmeaOut.tx.size() == 5 * gga.size());
    link.setNmeaOut(nullptr, 0, 0);
    serial.rx = gga;
    link.pollReceiver();
    CHECK(nmeaOut.tx.size() == 5 * gga.size());

    return finish("gnss_link");
}
