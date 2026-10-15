// Finds a TM171 IMU and a GNSS receiver among the USB serial devices (/dev/serial/by-id links to
// /dev/ttyACMx and /dev/ttyUSBx) by listening to each in turn: IMU packets make it the IMU,
// NMEA sentences with good checksums the receiver. A ttyUSB device (a USB to serial adapter)
// is listened to at 115200 and then 460800; for ttyACM the rate means nothing. Never writes to
// a device. Runs on the control thread from the 100 Hz tick; a device is listened to for up to
// 3 s, and the list is read again at most once a second.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "aow/imu.h"
#include "aow/nmea.h"
#include "linux_io.h"

namespace aow {

class UsbScout {
public:
    enum class Kind { None, Imu, Gnss };

    // What to look for. Turning a kind on again (its device went away) starts a new search.
    void want(Kind k, bool on);
    bool wants(Kind k) const { return k == Kind::Imu ? wantImu_ : k == Kind::Gnss ? wantGnss_ : false; }
    // Devices that are not to be listened to (the GNSS, radio, Keya and other ports in use),
    // and the ones the caller has taken. Compared by the device they point to.
    void exclude(const std::string& path);
    void release(const std::string& path);  // a device given back (unplugged)

    // From the tick. Returns a kind when a device has been identified; its path is then in
    // found(). The scout has closed it and the caller opens it for itself.
    Kind tick(uint64_t nowUs);
    const std::string& found() const { return found_; }
    unsigned foundBaud() const { return foundBaud_; }  // what it was heard at (USB adapters)

private:
    void next(uint64_t nowUs);

    bool wantImu_ = false, wantGnss_ = false;
    struct Excluded { std::string name, real; };  // real: what it pointed to when excluded
    std::vector<Excluded> excluded_;
    bool isExcluded(const std::string& real) const;
    struct Candidate { std::string path; unsigned baud; };
    std::vector<Candidate> candidates_;
    size_t nextCandidate_ = 0;
    uint64_t listedUs_ = 0, openedUs_ = 0;
    bool listed_ = false;
    linux_io::SerialPort port_;
    Candidate current_;
    std::string found_;
    unsigned foundBaud_ = 0;
    imu::Imu imu_;
    nmea::SentenceReader nmea_;
    int sentences_ = 0;
};

// The real path a device name points to, or "" if it does not exist.
std::string realDevice(const std::string& path);

}  // namespace aow
