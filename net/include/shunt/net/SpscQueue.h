// Lock-free single-producer single-consumer ring buffer.
#pragma once
#include <atomic>
#include <array>
#include <cstddef>

namespace shunt::net {

template <typename T, size_t Capacity = 4096>
class SpscQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
public:
    bool push(const T& v) {
        const size_t w = write_.load(std::memory_order_relaxed);
        const size_t r = read_.load(std::memory_order_acquire);
        if (w - r >= Capacity) { ++dropped_; return false; }
        buf_[w & (Capacity - 1)] = v;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& out) {
        const size_t r = read_.load(std::memory_order_relaxed);
        const size_t w = write_.load(std::memory_order_acquire);
        if (r == w) return false;
        out = buf_[r & (Capacity - 1)];
        read_.store(r + 1, std::memory_order_release);
        return true;
    }
    size_t size() const {
        return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire);
    }
    size_t dropped() const { return dropped_; }

private:
    std::array<T, Capacity> buf_{};
    alignas(64) std::atomic<size_t> write_{0};
    alignas(64) std::atomic<size_t> read_{0};
    size_t dropped_ = 0;
};

} // namespace shunt::net
