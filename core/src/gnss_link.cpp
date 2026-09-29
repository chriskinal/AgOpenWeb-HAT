#include "aow/gnss_link.h"

#include "aow/nav.h"

namespace aow {

GnssLink::GnssLink(hal::SerialPort& receiver, hal::UdpLink& app, hal::Clock& clock)
    : receiver_(receiver), app_(app), clock_(clock) {}

void GnssLink::pollReceiver() {
    uint8_t buf[1024];
    int n;
    while ((n = receiver_.read(buf, sizeof buf)) > 0) {
        for (int i = 0; i < n; ++i) {
            if (!reader_.feed(static_cast<char>(buf[i]))) continue;
            const uint64_t now = clock_.nowUs();
            const gnss::Message m = parser_.parse(reader_.sentence(), now);
            if (m == gnss::Message::None) continue;
            ++counters_.sentences;
            if (!gnss::carriesPosition(m)) continue;

            char out[256];
            const size_t len = nav::build(parser_.state(), now, out, sizeof out);
            if (!len) continue;
            if (app_.send(reinterpret_cast<const uint8_t*>(out), len)) ++counters_.navSent;
            else ++counters_.navSendErrors;
        }
    }
}

void GnssLink::forwardRtcm(const uint8_t* data, size_t len) {
    if (len < 5) return;
    ++counters_.rtcmPackets;
    const int written = receiver_.write(data, len);
    if (written < static_cast<int>(len))
        counters_.rtcmBytesDropped += static_cast<uint32_t>(len - (written > 0 ? written : 0));
}

}  // namespace aow
