// A mutex with priority inheritance, for locks the real-time control thread shares with the
// normal-priority web thread. While the web thread holds one, it runs at the control thread's
// priority, so the control loop never waits behind unrelated work. Use with std::lock_guard.
#pragma once

#include <pthread.h>

namespace aow {

class PiMutex {
public:
    PiMutex() {
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
        pthread_mutex_init(&m_, &a);
        pthread_mutexattr_destroy(&a);
    }
    ~PiMutex() { pthread_mutex_destroy(&m_); }
    PiMutex(const PiMutex&) = delete;
    PiMutex& operator=(const PiMutex&) = delete;

    void lock() { pthread_mutex_lock(&m_); }
    void unlock() { pthread_mutex_unlock(&m_); }

private:
    pthread_mutex_t m_;
};

}  // namespace aow
