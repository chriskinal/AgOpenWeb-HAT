// A snapshot of the daemon's state, published by the control loop for the web pages and the
// telemetry stream. Plain data: copied under a lock, never shared by reference.
#pragma once

#include <cstdint>

namespace aow {

struct Status {
    uint64_t uptimeMs = 0;

    // Steering (the fields the firmware's telemetry packet carries).
    float wasAngle = 0;        // deg
    float targetAngle = 0;     // deg, from PGN 254
    float currentAmps = 0;
    float speedKph = 0;        // from PGN 254
    float heading = 0;         // deg, GNSS
    bool autosteerActive = false;
    bool steerSwitch = false;
    bool workSwitch = false;
    bool haveAppLink = false;  // PGN 254 seen in the last 2 s

    // GNSS.
    uint8_t fixQuality = 0;
    uint8_t satellites = 0;
    uint32_t navSent = 0;

    // The four status LEDs (indicators::Color as numbers: 0 off, 1 red, 2 amber, 3 green, 4 blue).
    uint8_t leds[4] = {};
};

}  // namespace aow
