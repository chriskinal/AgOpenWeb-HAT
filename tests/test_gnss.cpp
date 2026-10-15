#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "aow/gnss.h"
#include "aow/nav.h"
#include "aow/nmea.h"
#include "check.h"

using namespace aow;

static bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

// Adds the checksum and CRLF to "$..." or "#..." text.
static std::string seal(const std::string& body) {
    char tail[16];
    if (body[0] == '#')
        std::snprintf(tail, sizeof tail, "*%08x\r\n", nmea::crc32(body.data() + 1, body.size() - 1));
    else
        std::snprintf(tail, sizeof tail, "*%02X\r\n", nmea::xorChecksum(body.data() + 1, body.size() - 1));
    return body + tail;
}

// Feeds text; returns how many sentences completed with a good checksum, and hands each to
// the parser if one is given.
static int feed(nmea::SentenceReader& r, const std::string& text, gnss::Parser* p = nullptr,
                uint64_t now = 0, gnss::Message* last = nullptr) {
    int n = 0;
    for (char c : text) {
        if (!r.feed(c)) continue;
        ++n;
        if (p) {
            const gnss::Message m = p->parse(r.sentence(), now);
            if (last) *last = m;
        }
    }
    return n;
}

static std::string sentence(const char* buf, size_t n) { return std::string(buf, n); }

static bool checksumOk(const std::string& s) {
    const size_t star = s.find('*');
    if (s[0] != '$' || star == std::string::npos || s.size() != star + 5) return false;
    char hex[3];
    std::snprintf(hex, sizeof hex, "%02X", nmea::xorChecksum(s.data() + 1, star - 1));
    return s.compare(star + 1, 2, hex) == 0 && s.compare(star + 3, 2, "\r\n") == 0;
}

int main() {
    // --- Checksums and framing
    // Unicore CRC-32: reflected 0xEDB88320, init 0, no final XOR. Value checked against the
    // firmware's table implementation (calc_crc32.h), which agrees on 20000 random strings.
    CHECK(nmea::crc32("123456789", 9) == 0x2DFD2D88u);

    const std::string gga = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
    {
        nmea::SentenceReader r;
        CHECK(feed(r, "noise\r\n" + gga) == 1);
        CHECK(std::strcmp(r.sentence(), "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,") == 0);

        std::string bad = gga;
        bad[bad.find('*') + 2] = '8';
        CHECK(feed(r, bad) == 0 && r.badChecksums() == 1);

        CHECK(feed(r, "$GPGGA,1235" + gga) == 1);            // a '$' restarts a cut-off sentence
        CHECK(feed(r, "$GPGGA,123519,4807.038,N\r\n") == 0);  // no checksum: dropped
        CHECK(feed(r, "$" + std::string(600, 'A') + "*00\r\n") == 0 && r.overflows() == 1);
        CHECK(feed(r, seal("#INSPVAA,x;y")) == 1);             // Unicore CRC-32
        std::string badU = seal("#INSPVAA,x;y");
        badU[badU.find('*') + 1] ^= 1;
        CHECK(feed(r, badU) == 0);
    }

    // --- GGA -> state -> PANDA
    {
        nmea::SentenceReader r;
        gnss::Parser p;
        gnss::Message m = gnss::Message::None;
        const uint64_t t0 = 10'000'000;

        CHECK(nav::select(p.state(), t0) == nav::Sentence::None);  // nothing yet
        CHECK(feed(r, gga, &p, t0, &m) == 1 && m == gnss::Message::Gga);
        CHECK(gnss::carriesPosition(m));
        const gnss::State& s = p.state();
        CHECK(near(s.latitude, 48.0 + 7.038 / 60.0) && near(s.longitude, 11.0 + 31.0 / 60.0));
        CHECK(s.fixQuality == 1 && s.numSatellites == 8 && near(s.hdop, 0.9f) && near(s.altitude, 545.4f, 1e-3));
        CHECK(s.hasPosition && !s.hasDualHeading);

        feed(r, seal("$GPVTG,054.7,T,034.4,M,005.5,N,010.2,K"), &p, t0, &m);
        CHECK(m == gnss::Message::Vtg && !gnss::carriesPosition(m));
        CHECK(near(s.speedKnots, 5.5f, 1e-4) && near(s.headingTrue, 54.7f, 1e-4));

        CHECK(nav::select(s, t0 + 1'000'000) == nav::Sentence::Panda);
        char out[256];
        const size_t n = nav::build(s, nullptr, t0 + 1'000'000, out, sizeof out);
        const std::string panda = sentence(out, n);
        CHECK(panda.rfind("$PANDA,123519.0,4807.038000,N,01131.000000,E,1,8,0.9,545.400,0.0,5.500,65535,0,0,0*", 0) == 0);
        CHECK(checksumOk(panda));
        CHECK(nav::build(s, nullptr, t0 + 1'000'000, out, 40) == 0);  // too small: nothing, not a torn sentence

        // Stale after 5 s without a position sentence (VTG alone does not count).
        CHECK(nav::select(s, t0 + 5'000'001) == nav::Sentence::None);

        // Fix quality 0 -> no PANDA.
        feed(r, seal("$GNGGA,123520.00,4807.038,N,01131.000,E,0,00,99.9,545.4,M,46.9,M,,"), &p, t0);
        CHECK(!s.hasPosition && nav::select(s, t0) == nav::Sentence::None);
    }

    // --- GNS mode letters, south/west hemispheres, coordinate rounding
    {
        nmea::SentenceReader r;
        gnss::Parser p;
        feed(r, seal("$GNGNS,010203.40,3359.9999999,S,15112.5,W,RRR,20,0.6,30.1,20.0,1.2,0000,V"), &p, 1);
        const gnss::State& s = p.state();
        CHECK(s.fixQuality == 4 && s.numSatellites == 20 && near(s.ageDgps, 1.2f, 1e-4));
        CHECK(s.latitude < -33.99 && s.longitude < -151.2);
        char out[256];
        const std::string panda = sentence(out, nav::buildPanda(s, nullptr, out, sizeof out));
        // 59.9999999' rounds to 34 deg 00.000000', not 33 deg 60.000000'
        CHECK(panda.rfind("$PANDA,010203.4,3400.000000,S,15112.500000,W,4,20,0.6,", 0) == 0);
        CHECK(checksumOk(panda));
    }

    // --- HPR: dual heading turns the output into PAOGI
    {
        nmea::SentenceReader r;
        gnss::Parser p;
        feed(r, gga, &p, 1);
        gnss::Message m;
        feed(r, seal("$GNHPR,123519.00,271.25,-1.50,0.30,4,24,1.0,0000"), &p, 1, &m);
        CHECK(m == gnss::Message::Hpr);
        const gnss::State& s = p.state();
        CHECK(s.hasDualHeading && near(s.dualHeading, 271.25f, 1e-4) && near(s.dualRoll, -1.5f, 1e-4));
        CHECK(s.headingQuality == 4 && near(s.ageDgps, 1.0f, 1e-4));
        CHECK(nav::select(s, 1) == nav::Sentence::Paogi);
        char out[256];
        const std::string paogi = sentence(out, nav::build(s, nullptr, 1, out, sizeof out));
        CHECK(paogi.rfind("$PAOGI,123519.0,4807.038000,N,01131.000000,E,1,24,0.9,545.400,1.0,0.000,271.2,-1.50,0,0.00*", 0) == 0 ||
              paogi.rfind("$PAOGI,123519.0,4807.038000,N,01131.000000,E,1,24,0.9,545.400,1.0,0.000,271.3,-1.50,0,0.00*", 0) == 0);
        CHECK(checksumOk(paogi));
    }

    // --- KSXT: decimal-degree position, quality 3 -> RTK fixed, speed km/h -> knots
    {
        nmea::SentenceReader r;
        gnss::Parser p;
        gnss::Message m;
        feed(r, seal("$KSXT,20260928123519.50,11.516667,48.117300,545.40,90.50,2.25,90.00,18.52,0.10,3,3,1.0,24,22"),
             &p, 7, &m);
        CHECK(m == gnss::Message::Ksxt && gnss::carriesPosition(m));
        const gnss::State& s = p.state();
        CHECK(near(s.fixTime, 123519.5, 1e-6));
        CHECK(near(s.latitude, 48.1173) && near(s.longitude, 11.516667));
        CHECK(s.fixQuality == 4 && s.headingQuality == 4 && s.numSatellites == 24);
        CHECK(near(s.speedKnots, 18.52f * 0.539957f, 1e-3));
        CHECK(near(s.dualHeading, 90.5f, 1e-4) && near(s.dualRoll, 2.25f, 1e-4));
        CHECK(nav::select(s, 7) == nav::Sentence::Paogi);
    }

    // --- INSPVAXA (UM981): status and position type map to fix quality; attitude at 19-21
    {
        nmea::SentenceReader r;
        gnss::Parser p;
        gnss::Message m;
        const std::string body =
            "#INSPVAXA,ICOM1,0,55.5,FINESTEERING,2400,3618.500,0,0,0;"
            "INS_SOLUTION_GOOD,INS_RTKFIXED,48.1173,11.516667,545.4,0.0,3.0,4.0,0.0,"
            "1.25,-0.50,181.75,0.01,0.01,0.02,0.01,0.01,0.01,0.1,0.1,0.2,0,7";
        CHECK(feed(r, seal(body), &p, 9, &m) == 1);
        CHECK(m == gnss::Message::Inspvaxa);
        const gnss::State& s = p.state();
        CHECK(s.hasIns && s.hasPosition && s.hasDualHeading);
        CHECK(s.insAlignment == 3 && s.posType == 56 && s.fixQuality == 4);
        CHECK(near(s.insRoll, 1.25f, 1e-4) && near(s.insPitch, -0.5f, 1e-4) && near(s.insHeading, 181.75f, 1e-4));
        CHECK(near(s.dualHeading, 181.75f, 1e-4) && near(s.dualRoll, 1.25f, 1e-4));
        CHECK(near(s.ageDgps, 7.0f));
        CHECK(near(s.fixTime, 10000.5, 1e-6));  // 3618.5 s of week - 18 leap = 01:00:00.5
        char out[256];
        const std::string paogi = sentence(out, nav::build(s, nullptr, 9, out, sizeof out));
        CHECK(paogi.rfind("$PAOGI,010000.5,4807.038000,N,01131.000020,E,4,12,0.9,545.400,7.0,", 0) == 0);
        CHECK(paogi.find(",181.8,1.25,-1,0.00*") != std::string::npos ||
              paogi.find(",181.7,1.25,-1,0.00*") != std::string::npos);  // pitch -0.5 rounds away from 0
        CHECK(checksumOk(paogi));

        // INS inactive: no position, but still PAOGI (the app shows the alignment state).
        const std::string inactive =
            "#INSPVAXA,ICOM1,0,55.5,FINESTEERING,2400,3619.000,0,0,0;"
            "INS_INACTIVE,NONE,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0";
        feed(r, seal(inactive), &p, 10);
        CHECK(!s.hasPosition && s.fixQuality == 0 && s.insAlignment == 0);
        CHECK(nav::select(s, 10) == nav::Sentence::Paogi);

        // Too few fields: ignored, state untouched.
        CHECK(p.parse("#INSPVAXA,ICOM1,0;INS_SOLUTION_GOOD", 11) == gnss::Message::None);
    }

    // --- INSPVAA (UM981)
    {
        gnss::Parser p;
        const std::string s1 =
            "#INSPVAA,COM1,0,50,FINESTEERING,2400,90000.000,0,0,18;"
            "2400,90000.000,-33.5,151.25,40.0,1.0,0.0,0.0,0.5,1.5,45.0,INS_ALIGNMENT_COMPLETE";
        CHECK(p.parse(s1.c_str(), 1) == gnss::Message::Inspvaa);
        const gnss::State& s = p.state();
        CHECK(s.hasIns && s.hasPosition && s.fixQuality == 5 && s.insAlignment == 7);
        CHECK(near(s.speedKnots, 1.94384f, 1e-4));
        CHECK(near(s.dualHeading, 45.0f) && near(s.dualRoll, 0.5f) && near(s.insPitch, 1.5f));
        CHECK(near(s.fixTime, 5942.0, 1e-6));  // 90000 - 18 = 89982 s -> day 1, 00:59:42
    }

    // --- Unknown and malformed sentences change nothing
    {
        gnss::Parser p;
        CHECK(p.parse("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W", 1) == gnss::Message::None);
        CHECK(p.parse("$GPGGA,123519", 1) == gnss::Message::None);
        CHECK(p.parse("", 1) == gnss::Message::None);
        CHECK(p.state().messageMask == 0);
    }

    return finish("gnss");
}
