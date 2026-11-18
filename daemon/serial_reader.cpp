// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors

#include "serial_reader.h"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

namespace aow {

SerialReader::SerialReader() {
    wakeFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    cmdFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    thread_ = std::thread([this] { run(); });
}

SerialReader::~SerialReader() {
    stop_ = true;
    poke(cmdFd_);
    if (thread_.joinable()) thread_.join();
    for (Slot& s : slots_)
        if (s.fd >= 0) ::close(s.fd);
    ::close(wakeFd_);
    ::close(cmdFd_);
}

void SerialReader::poke(int fd) {
    const uint64_t one = 1;
    (void)!::write(fd, &one, sizeof one);
}

int SerialReader::attach(int fd) {
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = slots_[i];
        if (s.state.load(std::memory_order_acquire) != Free) continue;
        s.fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (s.fd < 0) return -1;
        s.head.store(0, std::memory_order_relaxed);
        s.tail.store(0, std::memory_order_relaxed);
        s.dropped.store(0, std::memory_order_relaxed);
        s.state.store(Attaching, std::memory_order_release);
        poke(cmdFd_);
        return i;
    }
    return -1;
}

void SerialReader::detach(int slot) {
    if (slot < 0 || slot >= kSlots) return;
    Slot& s = slots_[slot];
    const int st = s.state.load(std::memory_order_acquire);
    if (st == Free || st == Detaching) return;
    s.state.store(Detaching, std::memory_order_release);
    poke(cmdFd_);
}

bool SerialReader::released(int slot) const {
    return slot < 0 || slot >= kSlots || slots_[slot].state.load(std::memory_order_acquire) == Free;
}

bool SerialReader::failed(int slot) const {
    return slot >= 0 && slot < kSlots && slots_[slot].state.load(std::memory_order_acquire) == Failed;
}

uint32_t SerialReader::dropped(int slot) const {
    return slot >= 0 && slot < kSlots ? slots_[slot].dropped.load(std::memory_order_relaxed) : 0;
}

void SerialReader::clearWake() {
    uint64_t n;
    (void)!::read(wakeFd_, &n, sizeof n);
}

size_t SerialReader::take(int slot, uint8_t* buf, size_t cap) {
    if (slot < 0 || slot >= kSlots) return 0;
    Slot& s = slots_[slot];
    const size_t tail = s.tail.load(std::memory_order_relaxed);
    const size_t n = std::min(cap, s.head.load(std::memory_order_acquire) - tail);
    for (size_t i = 0; i < n; ++i) buf[i] = s.ring[(tail + i) & (kRing - 1)];
    s.tail.store(tail + n, std::memory_order_release);
    return n;
}

// Only an active slot fails: one the control thread is detaching stays Detaching, to be freed.
void SerialReader::markFailed(Slot& s) {
    int active = Active;
    s.state.compare_exchange_strong(active, Failed, std::memory_order_acq_rel);
}

void SerialReader::readSlot(Slot& s) {
    uint8_t buf[1024];
    for (;;) {
        const ssize_t n = ::read(s.fd, buf, sizeof buf);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            markFailed(s);
            return;
        }
        if (n < 0) return;  // nothing more for now
        const size_t head = s.head.load(std::memory_order_relaxed);
        const size_t space = kRing - (head - s.tail.load(std::memory_order_acquire));
        const size_t keep = std::min(static_cast<size_t>(n), space);
        for (size_t i = 0; i < keep; ++i) s.ring[(head + i) & (kRing - 1)] = buf[i];
        s.head.store(head + keep, std::memory_order_release);
        if (keep < static_cast<size_t>(n)) s.dropped.fetch_add(static_cast<uint32_t>(n - keep), std::memory_order_relaxed);
    }
}

void SerialReader::run() {
    // Below the control loop (80) and above ordinary work, so the bytes still come promptly.
    sched_param sp{};
    sp.sched_priority = 70;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        sp.sched_priority = 0;
        pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
    }
    pthread_setname_np(pthread_self(), "aow-serial");

    pollfd fds[kSlots + 1];
    int slotOf[kSlots + 1];
    while (!stop_) {
        // Take up attaches and detaches, then poll the active slots and the command eventfd.
        int n = 0;
        fds[n++] = {cmdFd_, POLLIN, 0};
        for (int i = 0; i < kSlots; ++i) {
            Slot& s = slots_[i];
            int st = s.state.load(std::memory_order_acquire);
            if (st == Attaching) {
                s.state.store(Active, std::memory_order_release);
                st = Active;
            }
            if (st == Detaching) {
                ::close(s.fd);
                s.fd = -1;
                s.state.store(Free, std::memory_order_release);
                continue;
            }
            if (st != Active) continue;
            slotOf[n] = i;
            fds[n++] = {s.fd, POLLIN, 0};
        }
        if (poll(fds, static_cast<nfds_t>(n), -1) < 0) continue;  // EINTR
        if (fds[0].revents) {
            uint64_t v;
            (void)!::read(cmdFd_, &v, sizeof v);
        }
        bool news = false;
        for (int k = 1; k < n; ++k) {
            if (!fds[k].revents) continue;
            Slot& s = slots_[slotOf[k]];
            if (s.state.load(std::memory_order_acquire) != Active) continue;  // detached meanwhile
            if (fds[k].revents & (POLLERR | POLLHUP | POLLNVAL)) markFailed(s);
            else readSlot(s);
            news = true;
        }
        if (news) poke(wakeFd_);
    }
}

}  // namespace aow
