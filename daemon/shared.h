// State the control thread and the web thread both see. Each accessor copies under the lock;
// nothing is held across I/O.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include <sys/eventfd.h>
#include <unistd.h>

#include "aow/hal.h"
#include "aow/pi_mutex.h"
#include "aow/settings.h"
#include "aow/status.h"

namespace aow {

// A CAN frame the control thread read or wrote, for the GVRET stream.
struct MirrorFrame {
    uint8_t bus = 0;  // 0..2 = CAN1..3
    bool extended = false;
    uint8_t len = 0;
    uint32_t id = 0;
    uint32_t micros = 0;
    uint8_t data[8] = {};
};

class Shared {
public:
    Settings settings() const { std::lock_guard<PiMutex> l(mu_); return settings_; }
    void setSettings(const Settings& s) {
        { std::lock_guard<PiMutex> l(mu_); settings_ = s; }
        settingsVersion_.fetch_add(1);
    }
    // Bumped on every change, so the control loop can notice without taking the lock.
    unsigned settingsVersion() const { return settingsVersion_.load(); }

    Status status() const { std::lock_guard<PiMutex> l(mu_); return status_; }
    void setStatus(const Status& s) { std::lock_guard<PiMutex> l(mu_); status_ = s; }

    // For the ISOBUS thread, written by the control thread. Atomics: no lock at all. A change
    // also wakes the ISOBUS thread through an eventfd (a write that never blocks), so a section
    // goes to the implement without waiting for that thread's next pass.
    std::atomic<uint64_t> isoSections{0};       // section bits wanted, section 1 in bit 0
    std::atomic<bool> isoSectionControl{false}; // the app is there and wants sections driven
    void setIsoSections(bool control, uint64_t bits) {
        const bool changed = isoSectionControl.exchange(control, std::memory_order_relaxed) != control;
        if (isoSections.exchange(bits, std::memory_order_relaxed) != bits || changed) {
            const uint64_t one = 1;
            (void)!write(isoWakeFd_, &one, sizeof one);
        }
    }
    int isoWakeFd() const { return isoWakeFd_; }
    std::atomic<bool> isoAppLink{false};        // the app is sending PGN 254
    std::atomic<int32_t> isoXteMm{0};           // its cross-track error, for the terminal screen
    // From the ISOBUS thread, for PGN 240 to the app.
    std::atomic<bool> isoTcRunning{false};      // the task controller is on the bus
    std::atomic<bool> isoTcControl{false};      // it is sending setpoints to an implement
    std::atomic<uint8_t> isoTcSections{0};      // the implement's section count; 0 = none
    std::atomic<uint64_t> isoTcActual{0};       // sections it reports on, section 1 in bit 0
    std::atomic<int32_t> isoSpeedMmS{0};        // ground speed for the tractor ECU role

    // The ISOBUS thread publishes; the web thread reads.
    IsobusStatus isobus() const { std::lock_guard<PiMutex> l(isobusMu_); return isobus_; }
    void setIsobus(const IsobusStatus& s) { std::lock_guard<PiMutex> l(isobusMu_); isobus_ = s; }

    // The control thread changed the settings (PGN 251/252); the web thread saves them.
    void requestSave() { saveRequested_.store(true); }
    bool takeSaveRequest() { return saveRequested_.exchange(false); }

    // GVRET: the control thread queues frames only while a SavvyCAN client listens.
    std::atomic<bool> gvretListening{false};
    void pushMirror(const MirrorFrame& f) {
        std::lock_guard<PiMutex> l(mirrorMu_);
        if (mirrorCount_ == kMirror) {  // full: drop the oldest
            mirrorHead_ = (mirrorHead_ + 1) % kMirror;
            --mirrorCount_;
        }
        mirror_[(mirrorHead_ + mirrorCount_++) % kMirror] = f;
    }
    size_t takeMirror(MirrorFrame* out, size_t cap) {
        std::lock_guard<PiMutex> l(mirrorMu_);
        size_t n = 0;
        while (n < cap && mirrorCount_) {
            out[n++] = mirror_[mirrorHead_];
            mirrorHead_ = (mirrorHead_ + 1) % kMirror;
            --mirrorCount_;
        }
        return n;
    }

    // GNSS port hand-over for the UM98x page: the web thread requests, the control thread lets
    // go of the port (no reads, no RTCM) and confirms, the web thread talks to the receiver.
    std::atomic<hal::SerialPort*> gnssPort{nullptr};
    std::atomic<bool> gnssPauseRequested{false};
    std::atomic<bool> gnssPaused{false};

    // Times the CAN watch has restarted each channel's interface (can_watch.h).
    std::atomic<uint32_t> canRestarts[3] = {};

    // This unit's IPv4 addresses, network byte order, 0 = none; the web thread refreshes them
    // every second (for UdpLink::setOwnAddresses).
    static constexpr size_t kOwnAddresses = 8;
    std::atomic<uint32_t> ownAddresses[kOwnAddresses] = {};

    // A non-loopback interface is up (for the PWR LED); the web thread checks every second.
    std::atomic<bool> networkUp{false};

    std::atomic<bool> stop{false};  // set by either thread to end the daemon

private:
    mutable PiMutex mu_;
    Settings settings_;
    Status status_;
    const int isoWakeFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    mutable PiMutex isobusMu_;
    IsobusStatus isobus_;
    std::atomic<unsigned> settingsVersion_{0};
    std::atomic<bool> saveRequested_{false};
    static constexpr size_t kMirror = 1024;
    PiMutex mirrorMu_;
    MirrorFrame mirror_[kMirror];
    size_t mirrorHead_ = 0, mirrorCount_ = 0;
};

// The settings file. Save writes a temporary file and renames it over the old one, so a power
// cut never leaves a half-written file.
struct SettingsFile {
    std::string path;
    // Missing file: defaults, and true. Unreadable or broken: defaults, false, and a reason.
    bool load(Settings& s, std::string& error) const;
    bool save(const Settings& s, std::string& error) const;
    // The uploaded CAN brand file, beside the settings.
    std::string canConfigPath() const;
};

// Writes a temporary file, fsyncs it and renames it over path.
bool writeFileAtomically(const std::string& path, const std::string& data, std::string& error);

}  // namespace aow
