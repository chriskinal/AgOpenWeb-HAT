#include "aow/can/teensy_compat.h"

#include "aow/pgn.h"

namespace {
CanCompatHooks g;
}

CompatCanBus globalCAN1, globalCAN2, globalCAN3;

void canCompatSetup(const CanCompatHooks& hooks) {
    g = hooks;
    globalCAN1.bus = hooks.bus[0];
    globalCAN2.bus = hooks.bus[1];
    globalCAN3.bus = hooks.bus[2];
}

const CanCompatHooks& canCompatHooks() { return g; }

uint32_t millis() { return g.clock ? static_cast<uint32_t>(g.clock->nowUs() / 1000) : 0; }

static uint8_t busIndex(const CompatCanBus* b) { return b == &globalCAN1 ? 0 : b == &globalCAN2 ? 1 : 2; }

bool CompatCanBus::read(CAN_message_t& msg) {
    if (!bus) return false;
    aow::hal::CanFrame f;
    if (!bus->read(f)) return false;
    msg = CAN_message_t{};
    msg.id = f.id;
    msg.flags.extended = f.extended;
    msg.len = f.len > 8 ? 8 : f.len;
    std::memcpy(msg.buf, f.data, msg.len);
    if (g.mirror) g.mirror(busIndex(this), msg, false);
    return true;
}

bool CompatCanBus::write(const CAN_message_t& msg) {
    if (!bus) return false;
    aow::hal::CanFrame f;
    f.id = msg.id;
    f.extended = msg.flags.extended;
    f.len = msg.len > 8 ? 8 : msg.len;
    std::memcpy(f.data, msg.buf, f.len);
    const bool ok = bus->write(f);
    if (g.mirror) g.mirror(busIndex(this), msg, true);
    return ok;
}

void MessageBuilder::sendHardwarePopup(const char* message, uint8_t durationSeconds, uint8_t color) {
    if (!message || !*message) return;
    if (g.popup) g.popup(message, durationSeconds, color);
}
