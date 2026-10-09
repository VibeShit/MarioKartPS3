// Threading primitives for the PS3 (PSL1GHT) build.
//
// The PSL1GHT libstdc++ is configured with the "single" thread model, so the
// standard headers do not provide std::mutex, std::recursive_mutex,
// std::condition_variable or a usable std::thread constructor. The wrapper
// headers next to this file (<mutex>, <condition_variable>, <thread>) include
// the real libstdc++ headers and then add those missing pieces here, built on
// lv2 primitives. Mutexes are user-space locks so they stay constexpr
// constructible (static std::mutex objects are everywhere in the runtime) and
// never consume kernel objects.
#pragma once

#if !defined(__PPU__)
#error "ps3_threads.h is only for the PS3 PPU build"
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ps3compat {

// Implemented in compat/src/ps3_threads.cpp.
void Yield() noexcept;
void SleepMicros(uint64_t micros) noexcept;
uint64_t CurrentThreadId() noexcept;
uint64_t MonotonicMicros() noexcept;

// Starts `fn(arg)` on a new joinable PPU thread. Returns the lv2 thread id, or
// 0 on failure.
uint64_t StartThread(void (*fn)(void*), void* arg, uint64_t stackSize, const char* name) noexcept;
void JoinThread(uint64_t id) noexcept;
void DetachThread(uint64_t id) noexcept;

class SpinMutex {
public:
    constexpr SpinMutex() noexcept = default;
    SpinMutex(const SpinMutex&) = delete;
    SpinMutex& operator=(const SpinMutex&) = delete;

    void lock() noexcept {
        for (unsigned spins = 0;; ++spins) {
            if (!m_locked.exchange(true, std::memory_order_acquire))
                return;
            while (m_locked.load(std::memory_order_relaxed)) {
                if (spins < 64) {
                    ++spins;
                } else if (spins < 256) {
                    ++spins;
                    Yield();
                } else {
                    SleepMicros(20);
                }
            }
        }
    }
    bool try_lock() noexcept { return !m_locked.exchange(true, std::memory_order_acquire); }
    void unlock() noexcept { m_locked.store(false, std::memory_order_release); }

private:
    std::atomic<bool> m_locked{false};
};

class RecursiveSpinMutex {
public:
    constexpr RecursiveSpinMutex() noexcept = default;
    RecursiveSpinMutex(const RecursiveSpinMutex&) = delete;
    RecursiveSpinMutex& operator=(const RecursiveSpinMutex&) = delete;

    void lock() noexcept {
        const uint64_t self = CurrentThreadId();
        if (m_owner.load(std::memory_order_relaxed) == self) {
            ++m_depth;
            return;
        }
        m_inner.lock();
        m_owner.store(self, std::memory_order_relaxed);
        m_depth = 1;
    }
    bool try_lock() noexcept {
        const uint64_t self = CurrentThreadId();
        if (m_owner.load(std::memory_order_relaxed) == self) {
            ++m_depth;
            return true;
        }
        if (!m_inner.try_lock())
            return false;
        m_owner.store(self, std::memory_order_relaxed);
        m_depth = 1;
        return true;
    }
    void unlock() noexcept {
        if (--m_depth == 0) {
            m_owner.store(0, std::memory_order_relaxed);
            m_inner.unlock();
        }
    }

private:
    SpinMutex m_inner;
    std::atomic<uint64_t> m_owner{0};
    unsigned m_depth = 0;
};

// Sequence-counter condition variable. Waiters sleep in short slices and
// re-check the counter; spurious wakeups are permitted by the standard.
class SeqCondition {
public:
    constexpr SeqCondition() noexcept = default;

    void notify_one() noexcept { m_seq.fetch_add(1, std::memory_order_release); }
    void notify_all() noexcept { m_seq.fetch_add(1, std::memory_order_release); }

    template <typename Lock>
    void wait(Lock& lock) {
        const uint32_t seen = m_seq.load(std::memory_order_acquire);
        lock.unlock();
        unsigned spins = 0;
        while (m_seq.load(std::memory_order_acquire) == seen) {
            if (++spins < 32)
                Yield();
            else
                SleepMicros(100);
        }
        lock.lock();
    }

    // Returns false on timeout.
    template <typename Lock>
    bool wait_until_micros(Lock& lock, uint64_t deadlineMicros) {
        const uint32_t seen = m_seq.load(std::memory_order_acquire);
        lock.unlock();
        bool signalled = true;
        unsigned spins = 0;
        while (m_seq.load(std::memory_order_acquire) == seen) {
            const uint64_t now = MonotonicMicros();
            if (now >= deadlineMicros) {
                signalled = false;
                break;
            }
            if (++spins < 32) {
                Yield();
            } else {
                const uint64_t left = deadlineMicros - now;
                SleepMicros(left < 100 ? left : 100);
            }
        }
        lock.lock();
        return signalled;
    }

private:
    std::atomic<uint32_t> m_seq{0};
};

template <typename Clock, typename Duration>
uint64_t DeadlineMicros(const std::chrono::time_point<Clock, Duration>& deadline) {
    const auto remaining = deadline - Clock::now();
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(remaining).count();
    return MonotonicMicros() + (micros > 0 ? static_cast<uint64_t>(micros) : 0);
}

} // namespace ps3compat
