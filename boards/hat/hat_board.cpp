// See hat_board.h.
#include "hat_board.h"

#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "aow/log.h"
#include "hat_pins.h"

namespace aow::hat {

namespace {

// Output line order inside outFd_. Bit n of the values mask is line n.
enum OutLine : unsigned { OutDir = 0, OutEn = 1, OutWdtEn = 2, OutWdi = 3, OutCount = 4 };
enum InLine : unsigned { InWork = 0, InEngage = 1, InRemote = 2, InCount = 3 };

// The ADC128S102 frame: 16 bits, the next channel's address in bits 13:11 of the first byte,
// the previous frame's 12-bit result in the reply. Nine frames read all eight channels.
constexpr size_t kAdcFrames = 9;
constexpr size_t kAdcBytes = kAdcFrames * 2;
constexpr uint32_t kAdcSpiHz = 2'000'000;

int openGpioLines(int chipFd, const unsigned* offsets, unsigned n, bool output, uint64_t initial, bool pullUp) {
    gpio_v2_line_request req{};
    for (unsigned i = 0; i < n; ++i) req.offsets[i] = offsets[i];
    req.num_lines = n;
    std::snprintf(req.consumer, sizeof req.consumer, "aow-hatd");
    req.config.flags = output ? GPIO_V2_LINE_FLAG_OUTPUT : GPIO_V2_LINE_FLAG_INPUT;
    if (!output && pullUp) req.config.flags |= GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
    if (output) {
        req.config.num_attrs = 1;
        req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
        req.config.attrs[0].attr.values = initial;
        req.config.attrs[0].mask = (1ULL << n) - 1;
    }
    if (ioctl(chipFd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) return -1;
    return req.fd;
}

}  // namespace

HatBoard::~HatBoard() {
    // Safe state on the way out: EN low, PWM 0, watchdog off. The service unit's ExecStopPost
    // repeats this for a crash.
    if (outFd_ >= 0) {
        setOutputs((1ULL << OutEn) | (1ULL << OutWdtEn), 1ULL << OutWdtEn);
        close(outFd_);
    }
    if (dutyFd_ >= 0) {
        writePwmFile(dutyFd_, 0);
        close(dutyFd_);
    }
    if (inFd_ >= 0) close(inFd_);
    if (chipFd_ >= 0) close(chipFd_);
    if (spiFd_ >= 0) close(spiFd_);
}

bool HatBoard::open(const HatOptions& opt) {
    opt_ = opt;

    // ADC
    spiFd_ = ::open(opt.spidev, O_RDWR | O_CLOEXEC);
    if (spiFd_ < 0) {
        LOG_ERROR(System, "HAT: %s: %s", opt.spidev, std::strerror(errno));
        return false;
    }
    uint8_t mode = SPI_MODE_3, bits = 8;
    uint32_t hz = kAdcSpiHz;
    if (ioctl(spiFd_, SPI_IOC_WR_MODE, &mode) < 0 || ioctl(spiFd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(spiFd_, SPI_IOC_WR_MAX_SPEED_HZ, &hz) < 0) {
        LOG_ERROR(System, "HAT: %s setup: %s", opt.spidev, std::strerror(errno));
        return false;
    }

    // GPIO
    chipFd_ = ::open(opt.gpiochip, O_RDWR | O_CLOEXEC);
    if (chipFd_ < 0) {
        LOG_ERROR(System, "HAT: %s: %s", opt.gpiochip, std::strerror(errno));
        return false;
    }
    const unsigned ins[InCount] = {static_cast<unsigned>(kSwWork), static_cast<unsigned>(kSwEngage),
                                   static_cast<unsigned>(kSwRemote)};
    inFd_ = openGpioLines(chipFd_, ins, InCount, false, 0, true);
    if (inFd_ < 0) {
        LOG_ERROR(System, "HAT: switch inputs GPIO %d/%d/%d: %s", kSwWork, kSwEngage, kSwRemote, std::strerror(errno));
        return false;
    }
    const unsigned outs[OutCount] = {static_cast<unsigned>(kSteerDir), static_cast<unsigned>(kSteerEn),
                                     static_cast<unsigned>(kWdtEn), static_cast<unsigned>(kWdtWdi)};
    outBits_ = 1ULL << OutWdtEn;  // DIR low, EN low, watchdog off (EN pin high), WDI low
    outFd_ = openGpioLines(chipFd_, outs, OutCount, true, outBits_, false);
    if (outFd_ < 0) {
        LOG_ERROR(System, "HAT: outputs GPIO %d/%d/%d/%d: %s", kSteerDir, kSteerEn, kWdtEn, kWdtWdi, std::strerror(errno));
        return false;
    }

    // PWM through sysfs: export, period, duty 0, polarity, enable.
    char path[256];
    std::snprintf(path, sizeof path, "%s/pwm%u", opt.pwmchip, opt.pwmChannel);
    if (access(path, F_OK) != 0) {
        std::snprintf(path, sizeof path, "%s/export", opt.pwmchip);
        const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
        if (fd < 0 || !writePwmFile(fd, opt.pwmChannel)) {
            LOG_ERROR(System, "HAT: %s: %s", path, std::strerror(errno));
            if (fd >= 0) close(fd);
            return false;
        }
        close(fd);
        usleep(50'000);  // udev sets the group on the new files
    }
    periodNs_ = opt.pwmHz ? 1'000'000'000UL / opt.pwmHz : 0;
    if (periodNs_ < 1000) {
        LOG_ERROR(System, "HAT: PWM frequency %u Hz out of range", opt.pwmHz);
        return false;
    }
    const char* files[] = {"duty_cycle", "period", "duty_cycle", "polarity", "enable"};
    const char* values[] = {"0", nullptr, "0", "normal", "1"};
    char periodStr[32];
    std::snprintf(periodStr, sizeof periodStr, "%lu", periodNs_);
    values[1] = periodStr;
    for (int i = 0; i < 5; ++i) {
        std::snprintf(path, sizeof path, "%s/pwm%u/%s", opt.pwmchip, opt.pwmChannel, files[i]);
        const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
        const bool ok = fd >= 0 && ::write(fd, values[i], std::strlen(values[i])) > 0;
        const int err = errno;
        if (fd >= 0) close(fd);
        if (!ok && !(i == 3 && err == EINVAL)) {  // polarity may be fixed on some chips
            LOG_ERROR(System, "HAT: %s <- %s: %s", path, values[i], std::strerror(err));
            return false;
        }
    }
    std::snprintf(path, sizeof path, "%s/pwm%u/duty_cycle", opt.pwmchip, opt.pwmChannel);
    dutyFd_ = ::open(path, O_WRONLY | O_CLOEXEC);
    if (dutyFd_ < 0) {
        LOG_ERROR(System, "HAT: %s: %s", path, std::strerror(errno));
        return false;
    }
    dutyNs_ = 0;

    LOG_INFO(System, "HAT: ADC %s, GPIO %s, PWM %s/pwm%u at %u Hz, reference %.2f V", opt.spidev, opt.gpiochip,
             opt.pwmchip, opt.pwmChannel, opt.pwmHz, static_cast<double>(opt.adcReferenceVolts));
    readAdc();
    return true;
}

bool HatBoard::writePwmFile(int fd, unsigned long value) {
    char s[32];
    const int n = std::snprintf(s, sizeof s, "%lu", value);
    return ::write(fd, s, static_cast<size_t>(n)) == n;
}

bool HatBoard::setOutputs(uint64_t mask, uint64_t bits) {
    outBits_ = (outBits_ & ~mask) | (bits & mask);
    gpio_v2_line_values v{};
    v.mask = mask;
    v.bits = bits & mask;
    return ioctl(outFd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) == 0;
}

bool HatBoard::readAdc() {
    uint8_t tx[kAdcBytes] = {}, rx[kAdcBytes] = {};
    for (size_t f = 0; f < kAdcFrames; ++f) tx[2 * f] = static_cast<uint8_t>(((f % 8) & 7) << 3);  // next channel
    spi_ioc_transfer t{};
    t.tx_buf = reinterpret_cast<uintptr_t>(tx);
    t.rx_buf = reinterpret_cast<uintptr_t>(rx);
    t.len = kAdcBytes;
    t.speed_hz = kAdcSpiHz;
    t.bits_per_word = 8;
    if (ioctl(spiFd_, SPI_IOC_MESSAGE(1), &t) < 0) {
        ++adcErrors_;
        if (!adcErrorLogged_) {
            adcErrorLogged_ = true;
            LOG_ERROR(System, "HAT: ADC read: %s", std::strerror(errno));
        }
        return false;
    }
    for (int ch = 0; ch < 8; ++ch) {
        const size_t f = static_cast<size_t>(ch) + 1;  // channel ch answers in the following frame
        adc_[ch] = static_cast<uint16_t>(((rx[2 * f] << 8) | rx[2 * f + 1]) & 0x0FFF);
    }
    return true;
}

steer::Inputs HatBoard::read(const Settings&) {
    steer::Inputs in;
    readAdc();
    in.wasCounts = adc_[kAdcWas];
    in.currentVolts = adc_[kAdcCurrent] * opt_.adcReferenceVolts / 4096.0f;

    gpio_v2_line_values v{};
    v.mask = (1ULL << InCount) - 1;
    if (ioctl(inFd_, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) == 0) {  // contact to ground: low = closed
        in.workSwitch = !(v.bits & (1ULL << InWork));
        in.steerSwitch = !(v.bits & (1ULL << InEngage));
        in.remoteSwitch = !(v.bits & (1ULL << InRemote));
    }
    return in;
}

void HatBoard::drive(const steer::Output& out) {
    if (outFd_ < 0) return;
    if (!out.enable) {
        // EN first: the gate, whatever the PWM does.
        if (enabled_) setOutputs(1ULL << OutEn, 0);
        enabled_ = false;
        if (dutyNs_ != 0 && dutyFd_ >= 0 && writePwmFile(dutyFd_, 0)) dutyNs_ = 0;
        return;
    }
    const bool dirHigh = out.pwm >= 0;
    if (dirHigh != dirHigh_ || !enabled_) {
        setOutputs(1ULL << OutDir, dirHigh ? 1ULL << OutDir : 0);
        dirHigh_ = dirHigh;
    }
    const int mag = out.pwm < 0 ? -out.pwm : out.pwm;
    const unsigned long duty = periodNs_ * static_cast<unsigned long>(mag > 255 ? 255 : mag) / 255;
    if (duty != dutyNs_ && dutyFd_ >= 0 && writePwmFile(dutyFd_, duty)) dutyNs_ = duty;
    if (!enabled_) {
        setOutputs(1ULL << OutEn, 1ULL << OutEn);
        enabled_ = true;
    }
}

void HatBoard::watchdog(bool enable) {
    if (outFd_ < 0) return;
    // STWD100 EN: low = running. Kick once first so the first timeout starts now.
    if (enable) kickWatchdog();
    setOutputs(1ULL << OutWdtEn, enable ? 0 : 1ULL << OutWdtEn);
    LOG_INFO(System, "HAT: watchdog %s", enable ? "on (1.6 s)" : "off");
}

void HatBoard::kickWatchdog() {
    if (outFd_ < 0) return;
    wdiHigh_ = !wdiHigh_;
    setOutputs(1ULL << OutWdi, wdiHigh_ ? 1ULL << OutWdi : 0);
}

float HatBoard::vinVolts() const { return adc_[kAdcVin] * opt_.adcReferenceVolts / 4096.0f / 0.0758f; }
float HatBoard::keyVolts() const { return adc_[kAdcKey] * opt_.adcReferenceVolts / 4096.0f / 0.0758f; }

}  // namespace aow::hat
