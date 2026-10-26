// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
//
// Every serial port the daemon uses, and what is on it (the Serial Communications page).
//
// The onboard receiver (--gnss) is always the receiver there. The two RS-232 ports (--rs232)
// and each USB serial device (/dev/serial/by-id, ttyACM and ttyUSB) are listened to and what
// is plugged in is found from what it sends (serial_detect.h): a GNSS receiver, an RTK radio
// or an IMU, on auto rate trying the common rates until one parses. Only the outputs are set:
// a port or USB device with NMEA out or RTCM out on it is written to, not listened to.
// --radio and --imu name devices that are taken as such without detection (bench, sim). A
// port given to a Keya motor (settings file) is left alone.
//
// Routing: the receiver in use is the onboard one while it sends, else the first other one
// sending; its sentences go to GnssLink and corrections to it. RTCM from radios goes to that
// receiver; the first IMU sending feeds the Imu. Nothing is ever written to a device before it
// is identified, except an output.
//
// Runs on the control thread: fds() before poll(), handle() after, tick() at 100 Hz.
#pragma once

#include <poll.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "aow/gnss_link.h"
#include "aow/imu.h"
#include "aow/serial_detect.h"
#include "aow/settings.h"
#include "aow/status.h"
#include "linux_io.h"

namespace aow {

class SerialHub {
public:
    struct Config {
        std::string onboard;           // the HAT's receiver; empty: none
        unsigned onboardBaud = 460800;
        std::string rs232[2];          // empty: not there
        std::string radio, imu;        // fixed: taken as such, not detected
        unsigned radioBaud = 115200;
        std::string keya;              // a port the Keya motor has: not opened
        bool usb = true;               // look for USB serial devices
    };

    SerialHub(const Config& c, GnssLink& gnss, imu::Imu& imu, hal::Clock& clock);

    // The outputs and rates. Live.
    void apply(const Settings::Ports& p);

    // The fds to poll for reading, appended; handle() takes the same entries back after poll.
    void fds(std::vector<pollfd>& out);
    void handle(const pollfd* fds, size_t n);
    void tick();

    // The UM98x page has the onboard receiver: its bytes are left unread, and corrections and
    // radio bytes are dropped (they would land in the middle of its commands).
    void setLent(bool lent) { lent_ = lent; }
    hal::SerialPort* onboardPort();  // for that page; nullptr: none

    void status(Status& st) const;
    const std::string& gnssName() const;  // the receiver in use; empty: none
    const std::string& imuName() const;   // the IMU in use

private:
    struct Channel {
        enum class Place { Onboard, Rs232, Usb, Fixed } place;
        std::string name;  // for the page: "Onboard", "Port 1", the USB name, the path
        std::string path;
        int index = -1;    // RS-232: 0, 1
        SerialDetector::Kind fixed = SerialDetector::Kind::Unknown;  // Onboard, Fixed
        unsigned fixedBaud = 0;
        unsigned listenPinned = 0;  // the rate detection is held at: USB CDC 115200, else 0 (auto)
        bool nmeaOut = false, rtcmOut = false;
        linux_io::SerialPort port;
        SerialDetector det;
        uint64_t nextOpenUs = 0;
        bool gone = false;  // a USB device unplugged: removed at the next tick
        SerialDetector::Kind kind() const { return fixed != SerialDetector::Kind::Unknown ? fixed : det.kind(); }
        bool output() const { return nmeaOut || rtcmOut; }
        bool sending(uint64_t nowUs, uint64_t within) const;
    };

    Channel& add(Channel::Place place, const std::string& name, const std::string& path);
    bool open(Channel& c);
    unsigned outBaud(const Channel& c) const;
    void read(Channel& c);
    void scanUsb();
    void route();
    void announce(Channel& c, SerialDetector::Kind before);

    GnssLink& gnss_;
    imu::Imu& imu_;
    hal::Clock& clock_;
    std::vector<std::unique_ptr<Channel>> channels_;
    std::vector<Channel*> polled_;  // the channel behind each fd handed out by fds()
    std::vector<std::string> taken_;  // real paths not to treat as USB devices
    Settings::Ports ports_;
    Channel* gnssIn_ = nullptr;  // the receiver in use
    Channel* imuIn_ = nullptr;
    bool lent_ = false;
    bool usb_ = true;
    uint64_t nextUsbScanUs_ = 0;
};

}  // namespace aow
