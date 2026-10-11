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
        break;
    case pgn::Scan:
        send(out, pgn::buildScanReply(pgn::SrcSteer, ip_, out, sizeof out));
        send(out, pgn::buildScanReply(pgn::SrcGps, ip_, out, sizeof out));
        break;
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

void AppLink::sendPopup(const char* message, uint8_t seconds, uint8_t color) {
    uint8_t out[128];
    send(out, pgn::buildHardwareMessage(message, seconds, color, out, sizeof out));
}

void AppLink::send(const uint8_t* frame, size_t len) {
    if (!len || !udp_.send(frame, len)) ++counters_.sendErrors;
}

}  // namespace aow
