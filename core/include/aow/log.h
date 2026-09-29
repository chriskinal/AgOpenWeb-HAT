// Event logger, ported from the firmware's EventLogger: syslog severities, the same sources,
// the same per-severity rate limits, a 100-entry history for the web viewer.
//
// Unlike the firmware, log() does no I/O: it formats, rate-limits and queues. A thread that is
// allowed to block (the web thread) calls drain() to hand queued entries to the outputs
// (console/journal, UDP syslog, log WebSocket). So the control loop can log safely.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>

#include "aow/pi_mutex.h"

namespace aow::log {

enum class Severity : uint8_t { Emergency, Alert, Critical, Error, Warning, Notice, Info, Debug };
enum class Source : uint8_t { System, Network, Gnss, Imu, Autosteer, Machine, Can, Config, User };

const char* severityName(Severity s);  // "EMERG", "ALERT", ... "DEBUG" (the firmware's short names)
const char* levelName(Severity s);     // "EMERGENCY", ... "DEBUG"
const char* sourceName(Source s);      // "SYS", "NET", "GNSS", ...
// Accepts either name form, any case. Returns false if unknown.
bool parseSeverity(const char* text, Severity& out);

// Persisted with the other settings. Field names follow the firmware's EventConfig and its
// web API ("serial" is the console, which here is stderr and so the systemd journal).
struct Config {
    Severity consoleLevel = Severity::Info;
    Severity udpLevel = Severity::Warning;
    bool consoleEnabled = true;
    bool udpEnabled = false;
    uint16_t syslogPort = 514;
    bool rateLimitDisabled = false;
};

struct Entry {
    uint32_t timestampMs = 0;  // since the daemon started
    uint32_t sequence = 0;     // the firmware's event counter at the time
    Severity severity = Severity::Info;
    Source source = Source::System;
    char message[128] = {};
};

class Logger {
public:
    static constexpr size_t kHistory = 100;  // the firmware's LOG_BUFFER_SIZE
    static constexpr size_t kPending = 256;  // entries waiting for drain(); oldest dropped beyond

    using ClockMs = std::function<uint32_t()>;
    explicit Logger(ClockMs clock = nullptr);

    void log(Severity sev, Source src, const char* fmt, ...) __attribute__((format(printf, 4, 5)));

    // Outputs, called from drain() for each queued entry that passes that output's level.
    struct Outputs {
        std::function<void(const Entry&)> console;
        std::function<void(const Entry&)> udp;
        std::function<void(const Entry&)> web;  // every entry, as the firmware's log WebSocket
    };
    // Hands queued entries to the outputs. Returns how many. Call from a thread that may block.
    size_t drain(const Outputs& out);

    // Until setStartupDone(): no rate limit, and the console gets every level (as the firmware).
    void setStartupDone();
    bool startup() const;

    Config config() const;
    void setConfig(const Config& c);

    uint32_t eventCount() const;
    void resetEventCount();
    uint32_t droppedByRateLimit() const;
    uint32_t droppedPending() const;  // queue overflowed before drain()

    // Copies the history, oldest first. Returns the count.
    size_t history(Entry* out, size_t cap) const;

private:
    bool allow(Severity sev, uint32_t now);  // token bucket; caller holds mu_

    ClockMs clock_;
    mutable PiMutex mu_;  // shared with the real-time control thread
    Config config_;
    bool startup_ = true;
    uint32_t events_ = 0, rateDropped_ = 0, pendingDropped_ = 0;

    struct Bucket { float tokens; uint32_t lastMs; };
    Bucket buckets_[8];

    Entry history_[kHistory];
    size_t historyHead_ = 0, historyCount_ = 0;
    Entry pending_[kPending];
    size_t pendingHead_ = 0, pendingCount_ = 0;
};

// The daemon's logger, for the LOG_ macros.
Logger& global();

}  // namespace aow::log

#define AOW_LOG(sev, src, ...) ::aow::log::global().log(::aow::log::Severity::sev, ::aow::log::Source::src, __VA_ARGS__)
#define LOG_ERROR(src, ...) AOW_LOG(Error, src, __VA_ARGS__)
#define LOG_WARNING(src, ...) AOW_LOG(Warning, src, __VA_ARGS__)
#define LOG_NOTICE(src, ...) AOW_LOG(Notice, src, __VA_ARGS__)
#define LOG_INFO(src, ...) AOW_LOG(Info, src, __VA_ARGS__)
#define LOG_DEBUG(src, ...) AOW_LOG(Debug, src, __VA_ARGS__)
