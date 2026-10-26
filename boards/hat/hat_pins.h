// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
//
// AOW HAT pin map (BCM GPIO numbers).
// Source: hardware/hat/HARDWARE_HAT_DESIGN.md section 7. Change both together.
#pragma once

namespace aow::hat {

// Steering outputs (through 330 ohm to the motor driver)
inline constexpr int kSteerPwm = 18;  // hardware PWM0
inline constexpr int kSteerDir = 2;   // has a 1.8 k pull-up on the CM4: high at boot
inline constexpr int kSteerEn = 24;   // pulled down at boot: steering off

// Switch inputs, contact to ground, low = closed
inline constexpr int kSwWork = 17;
inline constexpr int kSwEngage = 27;
inline constexpr int kSwRemote = 22;

// SPI0: three MCP251863 CAN FD controllers and the ADC128S102
inline constexpr int kCsCan1 = 8;   // CE0
inline constexpr int kCsCan2 = 7;   // CE1
inline constexpr int kCsCan3 = 25;
inline constexpr int kCsAdc = 26;
inline constexpr int kIntCan1 = 16;
inline constexpr int kIntCan2 = 19;
inline constexpr int kIntCan3 = 20;

// Watchdog STWD100, 1.6 s
inline constexpr int kWdtEn = 3;   // low = watchdog on
inline constexpr int kWdtWdi = 6;

inline constexpr int kLedData = 21;  // 4 x SK6812, PCM
inline constexpr int kPiezo = 23;

// UARTs: UART2 = RS-232 #1 (GPIO0/1), UART3 = RS-232 #2 (GPIO4/5), UART5 = GNSS (GPIO12/13)

// ADC128S102 channels: reference = 5V_MAIN
inline constexpr int kAdcWas = 0;
inline constexpr int kAdcCurrent = 1;
inline constexpr int kAdcVin = 2;   // 0.0758 x V
inline constexpr int kAdcKey = 4;   // 0.0758 x V

}  // namespace aow::hat
