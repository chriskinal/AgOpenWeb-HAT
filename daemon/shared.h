// State the control thread and the web thread both see. Each accessor copies under the lock;
// nothing is held across I/O.
#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "aow/pi_mutex.h"
#include "aow/settings.h"
#include "aow/status.h"

namespace aow {

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

    // The control thread changed the settings (PGN 251/252); the web thread saves them.
    void requestSave() { saveRequested_.store(true); }
    bool takeSaveRequest() { return saveRequested_.exchange(false); }

    std::atomic<bool> stop{false};  // set by either thread to end the daemon

private:
    mutable PiMutex mu_;
    Settings settings_;
    Status status_;
    std::atomic<unsigned> settingsVersion_{0};
    std::atomic<bool> saveRequested_{false};
};

// The settings file. Save writes a temporary file and renames it over the old one, so a power
// cut never leaves a half-written file.
struct SettingsFile {
    std::string path;
    // Missing file: defaults, and true. Unreadable or broken: defaults, false, and a reason.
    bool load(Settings& s, std::string& error) const;
    bool save(const Settings& s, std::string& error) const;
};

}  // namespace aow
