#include "aow/steer_pgn.h"

#include "aow/pgn.h"

namespace aow::pgn {

bool decodeSteerCommand(const uint8_t* data, size_t len, SteerCommand& d) {
    if (!data || len < 8) return false;
    d.speedKmh = static_cast<uint16_t>(data[0] | data[1] << 8) * 0.1f;
    d.status = data[2];
    d.targetAngleDeg = static_cast<int16_t>(data[3] | data[4] << 8) * 0.01f;
    d.xte = static_cast<int8_t>(data[5]);
    d.sections = static_cast<uint16_t>(data[6] | data[7] << 8);
    return true;
}

size_t buildFromSteer(const SteerStatus& s, uint8_t* out, size_t cap) {
    const int16_t angle = static_cast<int16_t>(s.actualAngleDeg * 100.0f);
    // Bit 0 work switch, bit 1 steer switch, both inverted (1 = off); bit 2 remote/kickout.
    uint8_t switches = 0;
    if (!s.workSwitchOn) switches |= 0x01;
    if (!s.steerArmed) switches |= 0x02;
    if (s.remoteOrKickout) switches |= 0x04;
    const uint8_t data[8] = {
        static_cast<uint8_t>(angle & 0xFF), static_cast<uint8_t>((angle >> 8) & 0xFF),
        0, 0,  // heading: deprecated, sent by the GNSS
        0, 0,  // roll: deprecated
        switches, s.pwmDisplay,
    };
    return build(SrcSteer, FromSteer, data, sizeof data, out, cap);
}

size_t buildTurnSensor(uint8_t value, uint8_t* out, size_t cap) {
    const uint8_t data[8] = {value};
    return build(SrcSteer, TurnSensor, data, sizeof data, out, cap);
}

size_t buildHelloReply(uint8_t src, uint8_t* out, size_t cap) {
    const uint8_t data[5] = {};
    return build(src, src, data, sizeof data, out, cap);
}

size_t buildScanReply(uint8_t src, const uint8_t ip[4], uint8_t* out, size_t cap) {
    const uint8_t data[7] = {ip[0], ip[1], ip[2], ip[3], ip[0], ip[1], ip[2]};
    return build(src, ScanReply, data, sizeof data, out, cap);
}

}  // namespace aow::pgn
