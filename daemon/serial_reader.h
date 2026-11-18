// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The AgOpenWeb-HAT contributors
//
// Reads the serial ports on a thread of its own and hands the bytes to the control thread.
//
// Linux's tty layer makes poll() and read() on a port with nothing buffered wait for that port's
// receive work to finish (n_tty_poll and n_tty_read: tty_buffer_flush_work). The work runs in an
// ordinary kworker, with no priority inheritance, so whoever polls the port waits for that
// kworker to get a CPU. On the dev CM4 that held the FIFO 80 control loop up to 2.8 ms whenever
// other work (AgOpenWeb, nmcli, the Wi-Fi driver) crowded the kworker (docs/PLAN.md, 2026-11-17).
// Here only this thread waits; the control thread polls an eventfd and takes bytes from a ring.
//
// Each port is read through its own dup() of the control thread's fd, so the control thread can
// close or reopen its port at any time without this thread polling a reused fd number. Writes,
// opening and baud changes stay with the control thread: they do not wait on the kworker.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace aow {

class SerialReader {
public:
    static constexpr int kSlots = 16;

    SerialReader();  // starts the thread
    ~SerialReader();
    SerialReader(const SerialReader&) = delete;
    SerialReader& operator=(const SerialReader&) = delete;

    // Control thread only. attach() starts reading fd (duplicated here) and returns its slot, or
    // -1 with every slot in use or still being released. detach() stops it: the reader closes its
    // duplicate shortly after, and released() says when it has. Bytes not yet taken are dropped.
    int attach(int fd);
    void detach(int slot);
    bool released(int slot) const;

    // Readable when a slot has bytes or has failed. Poll it; clearWake() after.
    int wakeFd() const { return wakeFd_; }
    void clearWake();
    // Bytes read on slot so far, up to cap; 0 when none.
    size_t take(int slot, uint8_t* buf, size_t cap);
    // The port hung up or errored (an unplugged USB device): detach it.
    bool failed(int slot) const;
    // Bytes dropped on slot because the control thread had not taken them (ring full).
    uint32_t dropped(int slot) const;

private:
    enum State : int { Free, Attaching, Active, Failed, Detaching };
    static constexpr size_t kRing = 16 * 1024;  // a power of two; 0.35 s at 460800 baud

    struct Slot {
        std::atomic<int> state{Free};
        int fd = -1;  // the duplicate; written by attach() before Attaching, closed by the reader
        std::atomic<size_t> head{0}, tail{0};  // the reader advances head, the control thread tail
        std::atomic<uint32_t> dropped{0};
        uint8_t ring[kRing];
    };

    void run();
    void readSlot(Slot& s);
    void markFailed(Slot& s);
    void poke(int fd);

    Slot slots_[kSlots];
    int wakeFd_ = -1;  // reader to control thread: bytes or a failure
    int cmdFd_ = -1;   // control thread to reader: a slot attached or detached
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace aow
