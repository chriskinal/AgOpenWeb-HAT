// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors

#include "aow/app_link.h"

#include <cstring>

#include "aow/pgn.h"

namespace aow {

AppLink::AppLink(hal::UdpLink& udp, hal::Clock& clock, const uint8_t moduleIp[4])
    : udp_(udp), clock_(clock) {
    std::memcpy(ip_, moduleIp, sizeof ip_);
}

void AppLink::poll() {
    uint8_t buf[512];
    int n;
    while ((n = udp_.receive(buf, sizeof buf)) > 0) handle(buf, n);
}

uint64_t AppLink::usSinceCommand() const {
    return haveCommand_ ? clock_.nowUs() - commandAtUs_ : UINT64_MAX;
}

void AppLink::handle(const uint8_t* buf, int len) {
    pgn::Frame f;
    // Only frames from the host. Our own broadcasts, and other modules', are ignored.
    if (!pgn::parse(buf, static_cast<size_t>(len), f) || f.src != pgn::kSrcHost) {
        ++counters_.rejected;
        return;
    }
    ++counters_.received;
    anyFrame_ = true;
    anyFrameAtUs_ = clock_.nowUs();

    uint8_t out[32];
    switch (f.pgn) {
    case pgn::Hello:
        send(out, pgn::buildHelloReply(pgn::SrcSteer, out, sizeof out));
        send(out, pgn::buildHelloReply(pgn::SrcGps, out, sizeof out));
        if (imuPresent_) send(out, pgn::buildHelloReply(pgn::SrcImu, out, sizeof out));
        break;
    case pgn::Scan: {
        // The scanner's subnet, as the firmware. From loopback (the app on this unit) there is
        // nothing to move, so our own.
        uint8_t from[4];
        const uint8_t* subnet = udp_.lastSource(from) && from[0] != 127 ? from : ip_;
        send(out, pgn::buildScanReply(pgn::SrcSteer, ip_, subnet, out, sizeof out));
        send(out, pgn::buildScanReply(pgn::SrcGps, ip_, subnet, out, sizeof out));
        if (imuPresent_) send(out, pgn::buildScanReply(pgn::SrcImu, ip_, subnet, out, sizeof out));
        break;
    }
    case pgn::SteerData:
        if (!pgn::decodeSteerCommand(f.data, f.len, command_)) {
            ++counters_.rejected;
            return;
        }
        haveCommand_ = true;
        commandAtUs_ = clock_.nowUs();
        ++counters_.commands;
        if (steer_) steer_->onSteerData(command_);
        send(out, pgn::buildFromSteer(steer_ ? steer_->steerStatus() : status_, out, sizeof out));
        break;
    case pgn::SteerConfig:
        if (steer_) steer_->onSteerConfig(f.data, f.len);
        break;
    case pgn::SteerSettings:
        if (steer_) steer_->onSteerSettings(f.data, f.len);
        break;
    case pgn::CorrectedPosition: {
        pgn::CorrectedFix fix;
        if (!pgn::decodeCorrectedFix(f.data, f.len, fix)) {
            ++counters_.rejected;
            return;
        }
        ++counters_.fixes;
        if (fixSink_) fixSink_->onCorrectedFix(fix);
        break;
    }
    case pgn::VehicleGeometryData: {
        pgn::VehicleGeometry g;
        if (!pgn::decodeVehicleGeometry(f.data, f.len, g)) {
            ++counters_.rejected;
            return;
        }
        ++counters_.geometries;
        if (geometrySink_) geometrySink_->onVehicleGeometry(g);
        break;
    }
    case pgn::Sections64:
        if (f.len >= 8) {
            sections64_ = 0;
            for (int i = 0; i < 8; ++i) sections64_ |= static_cast<uint64_t>(f.data[i]) << (8 * i);
            haveSections64_ = true;
            sections64AtUs_ = clock_.nowUs();
        }
        break;
    case pgn::SectionControl:
        if (f.len >= 1) sectionControl_ = f.data[0] == 1;
        break;
    default:
        break;  // 239 and the rest: machine PGNs, no outputs on the HAT
    }
}

uint64_t AppLink::sections() const {
    // PGN 229 stops when the app goes back to 16 sections or fewer; a second without one and
    // PGN 254 is the source again.
    if (haveSections64_ && clock_.nowUs() - sections64AtUs_ < 1'000'000) return sections64_;
    return haveCommand_ ? command_.sections : 0;
}

void AppLink::sendTurnSensor(uint8_t value) {
    uint8_t out[16];
    send(out, pgn::buildTurnSensor(value, out, sizeof out));
}

void AppLink::sendSectionStatus(bool sectionControl, uint8_t count, uint64_t actual) {
    if (count > 64) count = 64;
    uint8_t data[10] = {static_cast<uint8_t>(sectionControl ? 1 : 0), count};
    const uint8_t bytes = static_cast<uint8_t>((count + 7) / 8);
    if (count < 64) actual &= (uint64_t{1} << count) - 1;
    for (uint8_t i = 0; i < bytes; ++i) data[2 + i] = static_cast<uint8_t>(actual >> (8 * i));
    uint8_t out[24];
    send(out, pgn::build(pgn::SrcTaskController, pgn::SectionStatus, data, static_cast<uint8_t>(2 + bytes), out, sizeof out));
}

void AppLink::sendPopup(const char* message, uint8_t seconds, uint8_t color) {
    uint8_t out[128];
    send(out, pgn::buildHardwareMessage(message, seconds, color, out, sizeof out));
}

void AppLink::send(const uint8_t* frame, size_t len) {
    if (!len || !udp_.send(frame, len)) ++counters_.sendErrors;
}

}  // namespace aow
