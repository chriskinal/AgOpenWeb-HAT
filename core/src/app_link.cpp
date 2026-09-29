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
        send(out, pgn::buildFromSteer(status_, out, sizeof out));
        break;
    default:
        break;  // 239, 251, 252 and the rest: handled as their phases land
    }
}

void AppLink::send(const uint8_t* frame, size_t len) {
    if (!len || !udp_.send(frame, len)) ++counters_.sendErrors;
}

}  // namespace aow
