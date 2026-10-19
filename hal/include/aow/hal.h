// Hardware interfaces. Core logic talks to hardware only through these, so it can be
// tested on a desktop with fakes. The HAT implementations live in boards/hat.
#pragma once

#include <cstddef>
#include <cstdint>

namespace aow::hal {

struct Clock {
    virtual ~Clock() = default;
    virtual uint64_t nowUs() = 0;  // monotonic
};

// 12-bit analog inputs, 0..4095 over the 5 V reference.
struct AnalogIn {
    enum Channel : uint8_t { Was = 0, Current = 1, Vin = 2, Key = 4 };
    virtual ~AnalogIn() = default;
    virtual bool read(Channel ch, uint16_t& counts) = 0;
};

struct SwitchInputs {
    virtual ~SwitchInputs() = default;
    virtual bool work() = 0;    // true = contact closed
    virtual bool engage() = 0;
    virtual bool remote() = 0;
};

// PWM + DIR + EN motor driver. enable(false) must always leave the motor unpowered.
struct MotorOut {
    virtual ~MotorOut() = default;
    virtual void enable(bool on) = 0;
    virtual void set(int16_t pwm) = 0;  // -255..255, sign = direction
};

struct SerialPort {
    virtual ~SerialPort() = default;
    virtual int read(uint8_t* buf, size_t cap) = 0;  // non-blocking, bytes read
    virtual int write(const uint8_t* buf, size_t len) = 0;
};

struct CanFrame {
    uint32_t id = 0;
    bool extended = false;
    uint8_t len = 0;
    uint8_t data[8] = {};
};

struct CanBus {
    virtual ~CanBus() = default;
    virtual bool read(CanFrame& f) = 0;  // non-blocking
    virtual bool write(const CanFrame& f) = 0;
};

struct UdpLink {
    virtual ~UdpLink() = default;
    virtual int receive(uint8_t* buf, size_t cap) = 0;  // non-blocking
    virtual bool send(const uint8_t* buf, size_t len) = 0;
    // The IPv4 address the last datagram receive() returned came from. False if not known.
    virtual bool lastSource(uint8_t ip[4]) const { (void)ip; return false; }
};

struct Watchdog {
    virtual ~Watchdog() = default;
    virtual void enable(bool on) = 0;
    virtual void kick() = 0;
};

}  // namespace aow::hal
