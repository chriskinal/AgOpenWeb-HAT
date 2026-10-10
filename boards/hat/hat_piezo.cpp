// See hat_piezo.h.
#include "hat_piezo.h"

#include <linux/gpio.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "aow/log.h"

namespace aow::hat {

namespace {

void addNs(timespec& t, long ns) {
    t.tv_nsec += ns;
    while (t.tv_nsec >= 1'000'000'000) {
        t.tv_nsec -= 1'000'000'000;
        ++t.tv_sec;
    }
}

bool before(const timespec& a, const timespec& b) {
    return a.tv_sec < b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec < b.tv_nsec);
}

}  // namespace

HatPiezo::~HatPiezo() {
    if (thread_.joinable()) {
        quit_ = true;
        ++generation_;
        const uint64_t one = 1;
        if (::write(wakeFd_, &one, sizeof one) < 0) {}
        thread_.join();
    }
    if (lineFd_ >= 0) {
        set(false);
        close(lineFd_);
    }
    if (wakeFd_ >= 0) close(wakeFd_);
}

bool HatPiezo::open(int chipFd, unsigned gpio) {
    gpio_v2_line_request req{};
    req.offsets[0] = gpio;
    req.num_lines = 1;
    std::snprintf(req.consumer, sizeof req.consumer, "aow-hatd piezo");
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;  // starts low: silent
    if (ioctl(chipFd, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
        LOG_ERROR(System, "HAT piezo: GPIO %u: %s", gpio, std::strerror(errno));
        return false;
    }
    lineFd_ = req.fd;
    wakeFd_ = eventfd(0, EFD_CLOEXEC);
    if (wakeFd_ < 0) {
        LOG_ERROR(System, "HAT piezo: eventfd: %s", std::strerror(errno));
        return false;
    }
    thread_ = std::thread([this] { run(); });
    return true;
}

void HatPiezo::play(const std::vector<indicators::Tone>& tones) {
    if (wakeFd_ < 0 || !thread_.joinable()) return;
    {
        std::lock_guard<PiMutex> lock(mu_);
        pending_ = tones;
    }
    ++generation_;
    const uint64_t one = 1;
    if (::write(wakeFd_, &one, sizeof one) < 0) {}
}

void HatPiezo::set(bool high) {
    gpio_v2_line_values v{};
    v.mask = 1;
    v.bits = high ? 1 : 0;
    ioctl(lineFd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
}

bool HatPiezo::sleepUntil(const timespec& t, uint32_t generation) {
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, nullptr);
    return generation_.load(std::memory_order_relaxed) == generation;
}

void HatPiezo::run() {
    // Above ordinary work so the tone is steady, far below the control loop (80).
    sched_param sp{};
    sp.sched_priority = 10;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        sp.sched_priority = 0;
        pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
    }
    for (;;) {
        uint64_t count;
        if (::read(wakeFd_, &count, sizeof count) < 0 && errno != EINTR) break;
        if (quit_) break;
        std::vector<indicators::Tone> tones;
        const uint32_t generation = generation_.load();
        {
            std::lock_guard<PiMutex> lock(mu_);
            tones.swap(pending_);
        }
        bool going = true;
        for (size_t i = 0; i < tones.size() && going; ++i) {
            const auto& tone = tones[i];
            timespec t, end;
            clock_gettime(CLOCK_MONOTONIC, &t);
            end = t;
            addNs(end, tone.ms * 1'000'000L);
            if (tone.hz) {
                const long half = 500'000'000L / tone.hz;
                bool high = false;
                while (going && before(t, end)) {
                    high = !high;
                    set(high);
                    addNs(t, half);
                    going = sleepUntil(t, generation);
                }
                set(false);
            } else {
                going = sleepUntil(end, generation);
            }
            if (going && tone.gapMs) {
                addNs(end, tone.gapMs * 1'000'000L);
                going = sleepUntil(end, generation);
            }
        }
        set(false);
    }
}

}  // namespace aow::hat
