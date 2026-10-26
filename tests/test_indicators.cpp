// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors

#include "aow/indicators.h"

#include "check.h"

using namespace aow::indicators;

int main() {
    Leds leds(1000);
    Inputs in;
    in.networkUp = true;
    in.appConnected = true;
    in.gnssData = true;
    in.fixQuality = 4;

    // Power: red for the first 5 s whatever else holds, then by network and app.
    in.nowMs = 5000;
    leds.update(in);
    CHECK(leds.color(Pwr) == Color::Red);
    in.nowMs = 6001;
    leds.update(in);
    CHECK(leds.color(Pwr) == Color::Green);
    in.appConnected = false;
    leds.update(in);
    CHECK(leds.color(Pwr) == Color::Yellow);
    in.networkUp = false;
    leds.update(in);
    CHECK(leds.color(Pwr) == Color::Red);

    // GPS: red without data, amber below RTK fixed, green at RTK fixed.
    CHECK(leds.color(Gps) == Color::Green);
    in.fixQuality = 5;
    leds.update(in);
    CHECK(leds.color(Gps) == Color::Yellow);
    in.gnssData = false;
    leds.update(in);
    CHECK(leds.color(Gps) == Color::Red);

    // Steer and INS.
    CHECK(leds.color(Steer) == Color::Yellow && leds.color(Ins) == Color::Off);
    in.motorActive = true;
    in.hasIns = true;
    in.insAlignment = 1;
    leds.update(in);
    CHECK(leds.color(Steer) == Color::Green && leds.color(Ins) == Color::Yellow);
    in.insAlignment = 3;
    leds.update(in);
    CHECK(leds.color(Ins) == Color::Green);

    // Colours at 100 %: red dimmed to 80 %, amber to 60 %, green full.
    in.gnssData = true;
    in.fixQuality = 4;
    in.networkUp = true;
    leds.update(in);
    CHECK(leds.rgb(Gps, 100, in.nowMs) == Rgb({0, 255, 0}));
    in.fixQuality = 1;
    leds.update(in);
    CHECK(leds.rgb(Gps, 100, in.nowMs) == Rgb({153, 76, 0}));  // 4095*.6 -> 153, 2048*.6 -> 76
    in.networkUp = false;
    leds.update(in);
    CHECK(leds.rgb(Pwr, 100, in.nowMs) == Rgb({204, 0, 0}));
    CHECK(leds.rgb(Pwr, 25, in.nowMs) == Rgb({50, 0, 0}));  // default brightness
    CHECK(leds.rgb(Ins, 25, in.nowMs).g > 0 && leds.rgb(Ins, 5, in.nowMs).g > 0);

    // RTCM pulse: blue for 50 ms, at most every 200 ms.
    leds.pulseRtcm(in.nowMs);
    CHECK(leds.rgb(Gps, 100, in.nowMs) == Rgb({0, 0, 255}));
    in.nowMs += 60;
    leds.update(in);
    CHECK(leds.rgb(Gps, 100, in.nowMs) == Rgb({153, 76, 0}));
    leds.pulseRtcm(in.nowMs);  // too soon
    CHECK(!(leds.rgb(Gps, 100, in.nowMs) == Rgb({0, 0, 255})));
    in.nowMs += 150;
    leds.pulseRtcm(in.nowMs);
    CHECK(leds.rgb(Gps, 100, in.nowMs) == Rgb({0, 0, 255}));

    // Button pulse on the steer LED.
    leds.pulseButton(in.nowMs);
    CHECK(leds.rgb(Steer, 100, in.nowMs + 49) == Rgb({0, 0, 255}));
    CHECK(!(leds.rgb(Steer, 100, in.nowMs + 50) == Rgb({0, 0, 255})));

    // Boot sound by volume.
    CHECK(bootChime(2).empty());
    CHECK(bootChime(0).size() == 1 && bootChime(0)[0].hz == 4000 && bootChime(0)[0].ms == 5);
    const auto loud = bootChime(1);
    CHECK(loud.size() == 5 && loud[2].hz == 2000 && loud[2].ms == 300 && loud[4].hz == 1000);

    return finish("indicators");
}
