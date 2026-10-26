// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
//
// Linux implementations of the hal interfaces that are not HAT hardware: the clock and the
// UDP link to the app.
#pragma once

#include <atomic>
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
    // Only datagrams from this host are delivered; the rest are counted and dropped. Another
    // AgOpenGPS app on the same network (a second tractor, a laptop) must not be able to arm
    // or disarm this unit's steering. nullptr accepts everyone.
    bool setAcceptFrom(const char* host);
    // This unit's own addresses (network byte order, 0 = unused slot), kept current by someone
    // else. A datagram from one of them is the app's broadcast copy of what it also sent on
    // loopback: still dropped, but not counted as another host's.
    void setOwnAddresses(const std::atomic<uint32_t>* addresses, size_t count) { own_ = addresses; ownCount_ = count; }
    int fd() const { return fd_; }

    int receive(uint8_t* buf, size_t cap) override;
    bool send(const uint8_t* buf, size_t len) override;
    bool lastSource(uint8_t ip[4]) const override;

    uint32_t ignored() const { return ignored_; }   // datagrams from other hosts
    uint32_t ownCopies() const { return ownCopies_; }  // dropped copies from our own addresses
    const char* lastIgnoredHost() const { return lastIgnored_; }

private:
    int fd_ = -1;
    sockaddr_in dest_{};
    bool haveDest_ = false;
    bool filter_ = false;
    in_addr accept_{};
    in_addr lastFrom_{};
    bool haveFrom_ = false;
    uint32_t ignored_ = 0;
    uint32_t ownCopies_ = 0;
    const std::atomic<uint32_t>* own_ = nullptr;
    size_t ownCount_ = 0;
    char lastIgnored_[16] = {};
};

// A SocketCAN interface (can0..2 on the HAT's MCP251863s; vcan for tests). Classic CAN only,
// as the firmware. Non-blocking.
class SocketCan : public hal::CanBus {
public:
    SocketCan() = default;
    ~SocketCan() override;
    SocketCan(const SocketCan&) = delete;
    SocketCan& operator=(const SocketCan&) = delete;

    // Sets the bit rate (not on vcan) and brings the interface up, then opens it. Returns false
    // and logs why if the interface is missing.
    bool open(const char* ifname, uint32_t bitrate);
    int fd() const { return fd_; }
    const char* name() const { return name_; }

    bool read(hal::CanFrame& f) override;
    bool write(const hal::CanFrame& f) override;
    uint32_t writeErrors() const { return writeErrors_; }

private:
    int fd_ = -1;
    char name_[16] = {};
    uint32_t writeErrors_ = 0;
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
    bool setBaud(unsigned baud);  // on an open port
    void close();                 // safe on a closed port
    int fd() const { return fd_; }

    int read(uint8_t* buf, size_t cap) override;
    int write(const uint8_t* buf, size_t len) override;

private:
    int fd_ = -1;
};

}  // namespace aow::linux_io
