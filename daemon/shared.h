// State the control thread and the web thread both see. Each accessor copies under the lock;
// nothing is held across I/O.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

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

    // For the ISOBUS thread, written by the control thread every tick. Atomics: no lock at all.
    std::atomic<uint64_t> isoSections{0};       // section bits wanted, section 1 in bit 0
    std::atomic<bool> isoSectionControl{false}; // the app is there and wants sections driven

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

    // A non-loopback interface is up (for the PWR LED); the web thread checks every second.
    std::atomic<bool> networkUp{false};

    std::atomic<bool> stop{false};  // set by either thread to end the daemon

private:
    mutable PiMutex mu_;
    Settings settings_;
    Status status_;
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
