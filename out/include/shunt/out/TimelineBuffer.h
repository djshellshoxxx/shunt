// Seqlock publication of the clock Timeline (ES-03 section 1): one writer (the
// engine thread), any number of lock-free readers (output threads, UI).
#pragma once
#include "shunt/clock/ClockEngine.h"
#include <atomic>
#include <cstring>
#include <type_traits>

namespace shunt::out {

class TimelineBuffer {
public:
    static_assert(std::is_trivially_copyable<clock::Timeline>::value, "Timeline must be trivially copyable");

    void write(const clock::Timeline& tl) {
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(static_cast<void*>(&data_), &tl, sizeof tl);
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);
    }

    clock::Timeline read() const {
        clock::Timeline out;
        for (;;) {
            const uint32_t a = seq_.load(std::memory_order_acquire);
            if (a & 1u) continue;
            std::memcpy(&out, static_cast<const void*>(&data_), sizeof out);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == a) return out;
        }
    }

private:
    std::atomic<uint32_t> seq_{0};
    clock::Timeline data_{};
};

} // namespace shunt::out
