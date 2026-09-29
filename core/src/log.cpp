#include "aow/log.h"

#include <strings.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace aow::log {

namespace {

// Messages per second allowed at each severity (the firmware's maxMessagesPerSecond).
constexpr uint8_t kRate[8] = {100, 100, 50, 50, 10, 10, 10, 5};
constexpr uint32_t kWindowMs = 1000;

const char* const kSeverity[8] = {"EMERG", "ALERT", "CRIT", "ERROR", "WARN", "NOTICE", "INFO", "DEBUG"};
const char* const kLevel[8] = {"EMERGENCY", "ALERT", "CRITICAL", "ERROR", "WARNING", "NOTICE", "INFO", "DEBUG"};
const char* const kSource[9] = {"SYS", "NET", "GNSS", "IMU", "STEER", "MACH", "CAN", "CFG", "USER"};

uint32_t steadyMs() {
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start).count());
}

}  // namespace

const char* severityName(Severity s) { return kSeverity[static_cast<int>(s) & 7]; }
const char* levelName(Severity s) { return kLevel[static_cast<int>(s) & 7]; }
const char* sourceName(Source s) {
    const auto i = static_cast<size_t>(s);
    return i < 9 ? kSource[i] : "?";
}

bool parseSeverity(const char* text, Severity& out) {
    for (int i = 0; i < 8; ++i) {
        if (!strcasecmp(text, kSeverity[i]) || !strcasecmp(text, kLevel[i])) {
            out = static_cast<Severity>(i);
            return true;
        }
    }
    return false;
}

Logger::Logger(ClockMs clock) : clock_(clock ? std::move(clock) : ClockMs(steadyMs)) {
    const uint32_t now = clock_();
    for (int i = 0; i < 8; ++i) buckets_[i] = {static_cast<float>(kRate[i]), now};
}

bool Logger::allow(Severity sev, uint32_t now) {
    if (startup_ || config_.rateLimitDisabled) return true;
    const int i = static_cast<int>(sev);
    Bucket& b = buckets_[i];
    const uint32_t elapsed = now - b.lastMs;
    if (elapsed > kWindowMs) {
        b.tokens = kRate[i];
        b.lastMs = now;
    } else if (elapsed > 0) {
        b.tokens = std::min(b.tokens + static_cast<float>(elapsed) * kRate[i] / kWindowMs,
                            static_cast<float>(kRate[i]));
        b.lastMs = now;
    }
    if (b.tokens < 1.0f) return false;
    b.tokens -= 1.0f;
    return true;
}

void Logger::log(Severity sev, Source src, const char* fmt, ...) {
    Entry e;
    e.severity = sev;
    e.source = src;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(e.message, sizeof e.message, fmt, ap);
    va_end(ap);

    const uint32_t now = clock_();
    std::lock_guard<PiMutex> lock(mu_);
    if (!allow(sev, now)) {
        ++rateDropped_;
        return;
    }
    e.timestampMs = now;
    e.sequence = ++events_;

    history_[historyHead_] = e;
    historyHead_ = (historyHead_ + 1) % kHistory;
    if (historyCount_ < kHistory) ++historyCount_;

    if (pendingCount_ == kPending) {  // drop the oldest
        pendingHead_ = (pendingHead_ + 1) % kPending;
        --pendingCount_;
        ++pendingDropped_;
    }
    pending_[(pendingHead_ + pendingCount_) % kPending] = e;
    ++pendingCount_;
}

size_t Logger::drain(const Outputs& out) {
    Entry batch[32];
    size_t total = 0;
    for (;;) {
        size_t n = 0;
        Config cfg;
        bool startup;
        {
            std::lock_guard<PiMutex> lock(mu_);
            while (n < 32 && pendingCount_) {
                batch[n++] = pending_[pendingHead_];
                pendingHead_ = (pendingHead_ + 1) % kPending;
                --pendingCount_;
            }
            cfg = config_;
            startup = startup_;
        }
        if (!n) return total;
        for (size_t i = 0; i < n; ++i) {
            const Entry& e = batch[i];
            if (out.console && cfg.consoleEnabled && (startup || e.severity <= cfg.consoleLevel)) out.console(e);
            if (out.udp && cfg.udpEnabled && e.severity <= cfg.udpLevel) out.udp(e);
            if (out.web) out.web(e);
        }
        total += n;
    }
}

void Logger::setStartupDone() {
    Severity level;
    {
        std::lock_guard<PiMutex> lock(mu_);
        if (!startup_) return;
        startup_ = false;
        level = config_.consoleLevel;
    }
    log(Severity::Info, Source::System, "System initialization complete - enforcing log level: %s",
        severityName(level));
}

bool Logger::startup() const {
    std::lock_guard<PiMutex> lock(mu_);
    return startup_;
}

Config Logger::config() const {
    std::lock_guard<PiMutex> lock(mu_);
    return config_;
}

void Logger::setConfig(const Config& c) {
    std::lock_guard<PiMutex> lock(mu_);
    config_ = c;
}

uint32_t Logger::eventCount() const {
    std::lock_guard<PiMutex> lock(mu_);
    return events_;
}

void Logger::resetEventCount() {
    std::lock_guard<PiMutex> lock(mu_);
    events_ = 0;
}

uint32_t Logger::droppedByRateLimit() const {
    std::lock_guard<PiMutex> lock(mu_);
    return rateDropped_;
}

uint32_t Logger::droppedPending() const {
    std::lock_guard<PiMutex> lock(mu_);
    return pendingDropped_;
}

size_t Logger::history(Entry* out, size_t cap) const {
    std::lock_guard<PiMutex> lock(mu_);
    const size_t n = std::min(cap, historyCount_);
    const size_t first = (historyHead_ + kHistory - historyCount_) % kHistory;
    const size_t skip = historyCount_ - n;  // keep the newest n
    for (size_t i = 0; i < n; ++i) out[i] = history_[(first + skip + i) % kHistory];
    return n;
}

Logger& global() {
    static Logger logger;
    return logger;
}

}  // namespace aow::log
