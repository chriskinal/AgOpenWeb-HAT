// Steer-module PGN payloads: what the app sends (254) and what a steer module answers (253,
// hello, scan reply). Layouts follow the Teensy firmware's AutosteerProcessor.
#pragma once

#include <cstddef>
#include <cstdint>

namespace aow::pgn {

// PGN 254, 100 Hz from the app.
struct SteerCommand {
    float speedKmh = 0;          // [0-1] uint16, 0.1 km/h
    uint8_t status = 0;          // [2]
    float targetAngleDeg = 0;    // [3-4] int16, 0.01 deg
    int8_t xte = 0;              // [5]
    uint16_t sections = 0;       // [6] sections 1-8, [7] sections 9-16

    bool guidanceActive() const { return status & 0x01; }
    bool autosteerEnabled() const { return status & 0x40; }
};

// Needs 8 data bytes. Returns false and leaves d untouched if the payload is short.
bool decodeSteerCommand(const uint8_t* data, size_t len, SteerCommand& d);

// PGN 253, one per PGN 254 received.
struct SteerStatus {
    float actualAngleDeg = 0;
    bool workSwitchOn = false;
    bool steerArmed = false;     // false = disarmed; the firmware's steerState 1
    bool remoteOrKickout = false;
    uint8_t pwmDisplay = 0;      // |PWM|, 0..255
};

// Each writes a complete frame into out and returns its length, or 0 if out is too small.
size_t buildFromSteer(const SteerStatus& s, uint8_t* out, size_t cap);

// PGN 250, 10 Hz: turn sensor reading (the firmware sends motor current, 0..255).
size_t buildTurnSensor(uint8_t value, uint8_t* out, size_t cap);

// PGN 221: a popup in the app. message up to 110 characters; color 0 info, 1 warning, 2 error.
size_t buildHardwareMessage(const char* message, uint8_t seconds, uint8_t color, uint8_t* out, size_t cap);

// Hello reply: PGN = src (126 steer, 120 GPS, 121 IMU), five zero data bytes.
size_t buildHelloReply(uint8_t src, uint8_t* out, size_t cap);

// PGN 203: the module's IPv4 address, then the first three octets of the address the scan came
// from, as AgOpenGPS's firmware (rem_ip): AgIO's Auto Set copies them into its subnet boxes.
size_t buildScanReply(uint8_t src, const uint8_t ip[4], const uint8_t subnet[3], uint8_t* out, size_t cap);

}  // namespace aow::pgn
