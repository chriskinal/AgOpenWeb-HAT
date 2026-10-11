// Persisted settings: the firmware's ConfigManager/EEPROM fields that apply to the HAT, with
// the firmware's defaults. Stored as JSON; a missing key keeps its default, so old files load.
#pragma once

#include <cstdint>
#include <string>

#include "aow/log.h"

namespace aow {

struct Settings {
    // Steer config: PGN 251 from the app, plus the two web-only fields.
    struct Steer {
        bool invertWas = false;
        bool motorDriveDirection = false;
        bool cytronDriver = false;
        bool steerSwitch = false;
        bool steerButton = false;
        bool currentSensor = false;
        uint8_t minSpeed = 3;             // 0.1 km/h
        uint8_t motorDriverConfig = 0;    // PGN 251 byte 8
        uint8_t currentThreshold = 100;   // PGN 251 byte 1 when the current sensor is on
        bool pwmBrakeMode = false;        // web: brake rather than coast
        uint16_t softStartDurationMs = 500;  // web, 0..1000
    } steer;

    // Steer settings: PGN 252 from the app.
    struct SteerGains {
        float kp = 40;
        uint8_t highPwm = 255;
        uint8_t minPwm = 10;
        uint8_t steerSensorCounts = 30;
        int16_t wasOffset = 0;
        float ackermanFix = 1.0f;
    } gains;

    struct Gps {
        bool passThrough = false;          // raw NMEA to the app instead of PANDA/PAOGI
        uint32_t radioBaud = 115200;       // RS-232 #1
    } gps;

    // CAN steering (the firmware's CANSteerConfig).
    struct Can {
        uint8_t brand = 0;                 // TractorBrand: 0 disabled
        struct Bus { uint8_t speed = 0; uint8_t function = 0; uint8_t name = 0; };  // speed 0 = 250k, 1 = 500k
        Bus bus[3];
        uint8_t moduleId = 0x1C;
    } can;

    // ISOBUS (docs/PLAN.md section 8). Applied at startup: a change needs a restart.
    struct Isobus {
        bool enabled = false;
        uint8_t channel = 2;               // 1..3; the firmware's ISO_Bus is CAN2
        // Stand in as the tractor ECU (ground speed on the bus) when the bus has none. It is
        // never started beside a real one, so this only needs switching off to keep speed
        // off the bus altogether.
        bool tractorEcu = true;
    } isobus;

    struct Misc {
        bool invertWorkSwitch = false;
        uint8_t ledBrightness = 25;        // %, 5..100
        uint8_t buzzerVolume = 1;          // 0 quiet, 1 loud, 2 off
    } misc;

    log::Config log;
};

// Serialises every field. Pretty-printed, so the file can be read and edited by hand.
std::string toJson(const Settings& s);

// Applies the keys present in json over s. Returns false (s untouched) if json does not parse.
bool fromJson(const std::string& json, Settings& s, std::string* error = nullptr);

}  // namespace aow
