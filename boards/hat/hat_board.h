// The AOW HAT as the daemon's Board: ADC128S102 over spidev, switches and the motor's DIR/EN
// through the GPIO character device, PWM0 through sysfs, the STWD100 watchdog lines.
// Pins: hat_pins.h. LEDs and the piezo are not driven yet (phase 7).
#pragma once

#include <cstdint>

#include "board.h"

namespace aow::hat {

struct HatOptions {
    const char* spidev = "/dev/spidev0.3";
    const char* gpiochip = "/dev/gpiochip0";
    const char* pwmchip = "/sys/class/pwm/pwmchip0";
    unsigned pwmChannel = 0;
    unsigned pwmHz = 20000;       // for MD13S / IBT-2 class drivers; the DRV8701 firmware used 75 Hz
    float adcReferenceVolts = 5.0f;  // the measured 5V_MAIN, for the non-ratiometric channels
};

class HatBoard : public Board {
public:
    HatBoard() = default;
    ~HatBoard() override;
    HatBoard(const HatBoard&) = delete;
    HatBoard& operator=(const HatBoard&) = delete;

    // Opens everything and leaves the outputs in their safe state: EN low, PWM 0, watchdog off.
    // Returns false and logs why on failure.
    bool open(const HatOptions& opt);

    const char* name() const override { return "hat"; }
    steer::Inputs read(const Settings& s) override;
    void drive(const steer::Output& out) override;
    void watchdog(bool enable) override;
    void kickWatchdog() override;

    // Telemetry: the last ADC conversion of each channel, 12-bit, and the derived volts.
    uint16_t adcCounts(int channel) const { return channel >= 0 && channel < 8 ? adc_[channel] : 0; }
    float vinVolts() const;  // the 12 V sense wire
    float keyVolts() const;  // the keyed 12 V
    uint32_t adcErrors() const { return adcErrors_; }

private:
    bool readAdc();
    bool setOutputs(uint64_t mask, uint64_t bits);
    bool writePwmFile(int fd, unsigned long value);

    HatOptions opt_;
    int spiFd_ = -1;
    int chipFd_ = -1;
    int inFd_ = -1;      // the three switch lines
    int outFd_ = -1;     // DIR, EN, WDT_EN, WDI
    int dutyFd_ = -1;    // pwmN/duty_cycle
    unsigned long periodNs_ = 0;
    unsigned long dutyNs_ = ~0UL;
    uint16_t adc_[8] = {};
    uint32_t adcErrors_ = 0;
    bool adcErrorLogged_ = false;
    bool enabled_ = false;
    bool dirHigh_ = false;
    bool wdiHigh_ = false;
    uint64_t outBits_ = 0;
};

}  // namespace aow::hat
