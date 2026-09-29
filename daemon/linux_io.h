// Linux implementations of the hal interfaces that are not HAT hardware: the clock and the
// UDP link to the app.
#pragma once

#include <netinet/in.h>

#include <cstdint>

#include "aow/hal.h"

namespace aow::linux_io {

struct MonotonicClock : hal::Clock {
    uint64_t nowUs() override;
};

// Receives on listenPort (all interfaces, so the app's subnet broadcasts arrive too) and
// sends to one fixed address. A link used only to receive (RTCM) gets no destination.
class UdpLink : public hal::UdpLink {
public:
    UdpLink() = default;
    ~UdpLink() override;
    UdpLink(const UdpLink&) = delete;
    UdpLink& operator=(const UdpLink&) = delete;

    // Each returns false and prints the reason on failure.
    bool open(uint16_t listenPort);
    bool setDestination(const char* host, uint16_t port);
    int fd() const { return fd_; }

    int receive(uint8_t* buf, size_t cap) override;
    bool send(const uint8_t* buf, size_t len) override;

private:
    int fd_ = -1;
    sockaddr_in dest_{};
    bool haveDest_ = false;
};

// A tty in raw 8N1 mode, non-blocking.
class SerialPort : public hal::SerialPort {
public:
    SerialPort() = default;
    ~SerialPort() override;
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    // Returns false and prints the reason on failure (including a baud rate termios lacks).
    bool open(const char* device, unsigned baud);
    int fd() const { return fd_; }

    int read(uint8_t* buf, size_t cap) override;
    int write(const uint8_t* buf, size_t len) override;

private:
    int fd_ = -1;
};

}  // namespace aow::linux_io
