// Persisted settings: the firmware's ConfigManager/EEPROM fields that apply to the HAT, with
// the firmware's defaults. Stored as JSON; a missing key keeps its default, so old files load.
#pragma once

#include <cstddef>
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
        bool useYAxis = false;            // PGN 251: the IMU is turned 90 degrees, roll and pitch swap
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
    } gps;  // the radio's rate: found by listening, or its port's (Ports::rate)

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
        // Show the status screen on the tractor's terminal (a virtual terminal on the bus).
        bool terminal = true;
    } isobus;

    // Serial ports (AOW HAT; the firmware's were fixed), the Serial Communications page. What
    // is plugged in is found from what it sends (serial_detect.h); only the outputs are set:
    // NMEA out and RTCM out, each on an RS-232 port or a USB serial device, both on one port if
    // wanted. A port with an output is not listened to. All of it applies live.
    struct Ports {
        enum Out : uint8_t { kNone, kPort1, kPort2, kUsb };
        enum Sentence : uint8_t { kGga = 1, kVtg = 2, kRmc = 4, kZda = 8, kHdt = 16 };
        static constexpr size_t kUsbName = 96;
        uint8_t nmeaOut = kNone, rtcmOut = kNone;
        char nmeaOutUsb[kUsbName] = {}, rtcmOutUsb[kUsbName] = {};  // its /dev/serial/by-id name
        uint32_t rate[2] = {0, 0};            // RS-232 #1, #2: 0 auto (found by listening)
        uint8_t nmeaSentences = kGga | kVtg;  // NMEA out: which of the receiver's sentences
        uint8_t nmeaRateHz = 0;               // 0: each as the receiver sends it; else 1, 2, 5, 10
        // A Keya serial motor (probed at startup, 115200): kPort1 or kPort2, settings file only.
        // That port is the Keya's and not listened to.
        uint8_t keya = kNone;

        static const char* outName(uint8_t o);  // "none", "port1", "port2", "usb"
        static bool parseOut(const char* name, uint8_t& o);
    } ports;

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

// Why these ports settings cannot be saved, or empty if they can.
std::string portsError(const Settings::Ports& p);

// The baud rates an RS-232 port can be pinned to.
bool validBaud(uint32_t baud);

}  // namespace aow
