// What the firmware's CAN files (CANConfigParser, CANProtocolEngine, TractorCANDriver) used
// from the Teensy build, reimplemented so they port near-verbatim:
//   CAN_message_t (FlexCAN_T4), millis(), constrain(), globalCAN1..3, MessageBuilder,
//   MotorDriverInterface, CANSteerConfig/CANFunction (ConfigManager) and the LOG_ macros.
//
// The three buses and the clock are wired up by the daemon (or a test) through canCompatSetup().
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>

#include "aow/hal.h"
#include "aow/log.h"

// --- FlexCAN_T4's message type (the fields the firmware uses)
struct CAN_message_t {
    uint32_t id = 0;
    struct { uint8_t extended = 0; uint8_t remote = 0; } flags;
    uint8_t len = 8;
    uint8_t buf[8] = {};
};

// --- Arduino
uint32_t millis();
template <typename T, typename L, typename H>
inline T constrain(T v, L lo, H hi) { return v < lo ? static_cast<T>(lo) : v > hi ? static_cast<T>(hi) : v; }

// --- The three buses (CAN1 = K_Bus, CAN2 = ISO_Bus, CAN3 = V_Bus by convention)
class CompatCanBus {
public:
    bool read(CAN_message_t& msg);
    bool write(const CAN_message_t& msg);
    aow::hal::CanBus* bus = nullptr;
};
extern CompatCanBus globalCAN1, globalCAN2, globalCAN3;

// Everything the CAN files reach outside themselves.
struct CanCompatHooks {
    aow::hal::Clock* clock = nullptr;
    aow::hal::CanBus* bus[3] = {};
    // PGN 221 popup to the app.
    std::function<void(const char* message, uint8_t durationSeconds, uint8_t color)> popup;
    // Every frame read or written, for the GVRET mirror. bus 0..2.
    std::function<void(uint8_t bus, const CAN_message_t& msg, bool tx)> mirror;
};
void canCompatSetup(const CanCompatHooks& hooks);
const CanCompatHooks& canCompatHooks();

// --- MessageBuilder (PGN 221)
class MessageBuilder {
public:
    static void sendHardwarePopup(const char* message, uint8_t durationSeconds, uint8_t color);
};

// --- ConfigManager's CAN types
enum class CANFunction : uint8_t { NONE = 0, STEERING = 1, BUTTONS = 2, HITCH = 4, IMPLEMENT = 8, KEYA = 16 };
struct CANSteerConfig {
    uint8_t brand = 0;
    uint8_t can1Speed = 0, can1Function = 0, can1Name = 0;
    uint8_t can2Speed = 0, can2Function = 0, can2Name = 0;
    uint8_t can3Speed = 0, can3Function = 0, can3Name = 0;
    uint8_t moduleID = 0x1C;
};

// --- MotorDriverInterface, trimmed to what TractorCANDriver overrides
enum class MotorDriverType { NONE, CYTRON_MD30C, IBT2, DRV8701, KEYA_CAN, KEYA_SERIAL, DANFOSS, GENERIC_PWM, TRACTOR_CAN };
enum class KickoutType { NONE, WHEEL_ENCODER, PRESSURE_SENSOR, CURRENT_SENSOR };
struct MotorStatus {
    bool enabled = false;
    int16_t targetPWM = 0;
    int16_t actualPWM = 0;
    float currentDraw = 0;
    uint32_t errorCount = 0;
    uint32_t lastUpdateMs = 0;
    bool hasError = false;
    char errorMessage[64] = {};
};
class MotorDriverInterface {
public:
    virtual ~MotorDriverInterface() = default;
    virtual bool init() = 0;
    virtual void enable(bool en) = 0;
    virtual void setPWM(int16_t pwm) = 0;
    virtual void stop() = 0;
    virtual MotorStatus getStatus() const = 0;
    virtual MotorDriverType getType() const = 0;
    virtual const char* getTypeName() const = 0;
    virtual bool hasCurrentSensing() const = 0;
    virtual bool hasPositionFeedback() const = 0;
    virtual void process() {}
    virtual bool isDetected() = 0;
    virtual void handleKickout(KickoutType type, float value) = 0;
    virtual float getCurrentDraw() = 0;
};

// --- EventLogger: the files log as EventSource::AUTOSTEER, as in the firmware.
#define FW_LOG(sev, ...) ::aow::log::global().log(::aow::log::Severity::sev, ::aow::log::Source::Autosteer, __VA_ARGS__)
