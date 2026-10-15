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
    } gps;  // the radio's baud rate is its port's, Ports::baud (older files: gps.radioBaud)

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

    // Where each serial device is (AOW HAT; the firmware's ports are fixed): the Serial
    // Communications page's matrix. Applied at startup, except the baud rates, which apply
    // live. Each RS-232 port holds one device at most; allowed() says which places suit which.
    struct Ports {
        enum Where : uint8_t { kNone, kPort1, kPort2, kUsb, kOnboard };
        enum Device : uint8_t { kGnss, kRadio, kImu, kKeya, kNmeaOut, kRtcmOut, kDevices };
        enum Sentence : uint8_t { kGga = 1, kVtg = 2, kRmc = 4, kZda = 8, kHdt = 16 };
        uint8_t where[kDevices] = {kOnboard, kPort1, kNone, kPort2, kNone, kNone};  // the firmware's layout
        uint32_t baud[2] = {115200, 115200};  // RS-232 #1, #2; an IMU or Keya there runs at 115200
        uint8_t nmeaSentences = kGga | kVtg;  // NMEA out: which of the receiver's sentences
        uint8_t nmeaRateHz = 0;               // 0: each as the receiver sends it; else 1, 2, 5, 10

        static bool allowed(Device d, Where w);
        static const char* deviceName(Device d);  // as in the settings file: "gnss", "radio", ...
        static const char* whereName(Where w);    // "none", "port1", "port2", "usb", "onboard"
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

// The device path and baud rate of each serial device, from the ports settings, the RS-232
// ports' device names (empty: not there) and the onboard receiver's. usb: to be found among
// the USB serial devices. Nothing for a device whose place is not there or not allowed, or
// for a second device on a port that already has one (a hand-edited file).
struct PortPlan {
    struct Dev {
        std::string path;  // empty: none (or USB)
        unsigned baud = 0;
        bool usb = false;
        int port = -1;     // 0, 1: on that RS-232 port
    };
    Dev dev[Settings::Ports::kDevices];
};
PortPlan planPorts(const Settings::Ports& p, const std::string rs232[2], const std::string& onboardGnss,
                   unsigned onboardBaud);

// Why these ports settings cannot be saved, or empty if they can.
std::string portsError(const Settings::Ports& p);

// The baud rates an RS-232 port offers (the firmware's radio list).
bool validBaud(uint32_t baud);

}  // namespace aow
