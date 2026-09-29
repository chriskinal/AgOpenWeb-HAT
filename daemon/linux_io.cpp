#include "linux_io.h"

#include <arpa/inet.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>

#include "aow/log.h"

namespace aow::linux_io {

uint64_t MonotonicClock::nowUs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000u + static_cast<uint64_t>(ts.tv_nsec) / 1000u;
}

UdpLink::~UdpLink() {
    if (fd_ >= 0) close(fd_);
}

bool UdpLink::setDestination(const char* host, uint16_t port) {
    dest_.sin_family = AF_INET;
    dest_.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &dest_.sin_addr) != 1) {
        LOG_ERROR(Network, "udp: bad address '%s'", host);
        return false;
    }
    haveDest_ = true;
    return true;
}

bool UdpLink::open(uint16_t listenPort) {
    fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        LOG_ERROR(Network, "udp: socket: %s", std::strerror(errno));
        return false;
    }
    const int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(listenPort);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof local) < 0) {
        LOG_ERROR(Network, "udp: bind port %u: %s", listenPort, std::strerror(errno));
        return false;
    }
    return true;
}

int UdpLink::receive(uint8_t* buf, size_t cap) {
    const ssize_t n = recv(fd_, buf, cap, 0);
    return n > 0 ? static_cast<int>(n) : 0;
}

bool UdpLink::send(const uint8_t* buf, size_t len) {
    if (!haveDest_) return false;
    return sendto(fd_, buf, len, 0, reinterpret_cast<const sockaddr*>(&dest_), sizeof dest_) ==
           static_cast<ssize_t>(len);
}

SocketCan::~SocketCan() {
    if (fd_ >= 0) close(fd_);
}

bool SocketCan::open(const char* ifname, uint32_t bitrate) {
    std::snprintf(name_, sizeof name_, "%s", ifname);
    std::ifstream type(std::string("/sys/class/net/") + ifname + "/type");
    int t = 0;
    if (!(type >> t) || t != 280) {  // ARPHRD_CAN
        LOG_WARNING(Can, "%s: no such CAN interface", ifname);
        return false;
    }
    // A real controller (vcan has no device) gets the configured bit rate. Needs root.
    std::ifstream dev(std::string("/sys/class/net/") + ifname + "/device/uevent");
    if (dev) {
        char cmd[160];
        std::snprintf(cmd, sizeof cmd,
                      "ip link set %s down && ip link set %s type can bitrate %u restart-ms 100 && ip link set %s up",
                      ifname, ifname, bitrate, ifname);
        if (std::system(cmd) != 0) LOG_WARNING(Can, "%s: could not set %u bit/s", ifname, bitrate);
    } else {
        std::string up = std::string("ip link set ") + ifname + " up 2>/dev/null";
        if (std::system(up.c_str()) != 0) {}  // vcan: usually up already
    }

    fd_ = socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, CAN_RAW);
    if (fd_ < 0) {
        LOG_ERROR(Can, "%s: socket: %s", ifname, std::strerror(errno));
        return false;
    }
    sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = static_cast<int>(if_nametoindex(ifname));
    if (!addr.can_ifindex || bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
        LOG_ERROR(Can, "%s: bind: %s", ifname, std::strerror(errno));
        close(fd_);
        fd_ = -1;
        return false;
    }
    return true;
}

bool SocketCan::read(hal::CanFrame& f) {
    can_frame fr;
    for (;;) {
        const ssize_t n = ::read(fd_, &fr, sizeof fr);
        if (n != static_cast<ssize_t>(sizeof fr)) return false;
        if (fr.can_id & (CAN_ERR_FLAG | CAN_RTR_FLAG)) continue;  // error and remote frames: skip
        f.extended = fr.can_id & CAN_EFF_FLAG;
        f.id = fr.can_id & (f.extended ? CAN_EFF_MASK : CAN_SFF_MASK);
        f.len = fr.can_dlc > 8 ? 8 : fr.can_dlc;
        std::memcpy(f.data, fr.data, f.len);
        return true;
    }
}

bool SocketCan::write(const hal::CanFrame& f) {
    can_frame fr{};
    fr.can_id = f.extended ? ((f.id & CAN_EFF_MASK) | CAN_EFF_FLAG) : (f.id & CAN_SFF_MASK);
    fr.can_dlc = f.len > 8 ? 8 : f.len;
    std::memcpy(fr.data, f.data, fr.can_dlc);
    if (::write(fd_, &fr, sizeof fr) == static_cast<ssize_t>(sizeof fr)) return true;
    ++writeErrors_;  // bus off, no ACK (nothing listening), or the queue is full
    return false;
}

namespace {

speed_t baudConstant(unsigned baud) {
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default: return B0;
    }
}

}  // namespace

SerialPort::~SerialPort() {
    if (fd_ >= 0) close(fd_);
}

bool SerialPort::open(const char* device, unsigned baud) {
    const speed_t speed = baudConstant(baud);
    if (speed == B0) {
        LOG_ERROR(Gnss, "serial: unsupported baud rate %u", baud);
        return false;
    }
    fd_ = ::open(device, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        LOG_ERROR(Gnss, "serial: %s: %s", device, std::strerror(errno));
        return false;
    }
    termios t{};
    if (tcgetattr(fd_, &t) < 0) {
        LOG_ERROR(Gnss, "serial: %s: not a tty: %s", device, std::strerror(errno));
        return false;
    }
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(CSTOPB | CRTSCTS);
    cfsetispeed(&t, speed);
    cfsetospeed(&t, speed);
    if (tcsetattr(fd_, TCSANOW, &t) < 0) {
        LOG_ERROR(Gnss, "serial: %s: %s", device, std::strerror(errno));
        return false;
    }
    tcflush(fd_, TCIOFLUSH);
    return true;
}

int SerialPort::read(uint8_t* buf, size_t cap) {
    const ssize_t n = ::read(fd_, buf, cap);
    return n > 0 ? static_cast<int>(n) : 0;
}

int SerialPort::write(const uint8_t* buf, size_t len) {
    const ssize_t n = ::write(fd_, buf, len);
    return n > 0 ? static_cast<int>(n) : 0;
}

}  // namespace aow::linux_io
