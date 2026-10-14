// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>

namespace sonora {

// Single-producer / single-consumer lock-free ring buffer.
//
// The audio callback is the producer while recording and the consumer while
// playing back. The disk thread takes the opposite role. Neither side ever
// blocks or allocates, so the callback stays real-time safe.
template <typename T>
class RingBuffer {
public:
    // capacity is rounded up to a power of two so the index wrap is a mask.
    explicit RingBuffer(std::size_t capacity)
        : capacity_(round_up_pow2(capacity)),
          mask_(capacity_ - 1),
          data_(new T[capacity_]) {
        std::memset(data_.get(), 0, capacity_ * sizeof(T));
    }

    std::size_t capacity() const { return capacity_; }

    // Consumer side. Acquire on write_ pairs with the producer's release
    // store, which is what makes the data it wrote visible to us. read_ is
    // ours, so a relaxed load of it is enough.
    std::size_t read_space() const {
        const std::size_t w = write_.load(std::memory_order_acquire);
        const std::size_t r = read_.load(std::memory_order_relaxed);
        return w - r;
    }

    // Producer side, and the roles are reversed: acquire goes on read_.
    //
    // This is not symmetry for its own sake. The consumer stores read_ with
    // release *after* its memcpy has finished. If we loaded read_ relaxed we
    // could observe the advanced index without synchronising with that copy,
    // and then overwrite bytes the consumer is still reading. ThreadSanitizer
    // caught exactly that, in CI, before any hardware did.
    std::size_t write_space() const {
        const std::size_t r = read_.load(std::memory_order_acquire);
        const std::size_t w = write_.load(std::memory_order_relaxed);
        return capacity_ - (w - r) - 1;
    }

    // Returns the number of items actually written (may be short).
    std::size_t write(const T* src, std::size_t count) {
        const std::size_t avail = write_space();
        if (count > avail) count = avail;
        const std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t first = std::min(count, capacity_ - (w & mask_));
        std::memcpy(data_.get() + (w & mask_), src, first * sizeof(T));
        if (count > first) {
            std::memcpy(data_.get(), src + first, (count - first) * sizeof(T));
        }
        write_.store(w + count, std::memory_order_release);
        return count;
    }

    // Returns the number of items actually read (may be short).
    std::size_t read(T* dst, std::size_t count) {
        const std::size_t avail = read_space();
        if (count > avail) count = avail;
        const std::size_t r = read_.load(std::memory_order_relaxed);
        const std::size_t first = std::min(count, capacity_ - (r & mask_));
        std::memcpy(dst, data_.get() + (r & mask_), first * sizeof(T));
        if (count > first) {
            std::memcpy(dst + first, data_.get(), (count - first) * sizeof(T));
        }
        read_.store(r + count, std::memory_order_release);
        return count;
    }

    // No reset(): a ring shared with the audio thread cannot be proven idle
    // on the other side, and an index stored under a writer mid-copy hands
    // the reader whatever the buffer held. Boundaries travel in band
    // instead (DiskThread, CaptureBoundary).

private:
    static std::size_t round_up_pow2(std::size_t n) {
        std::size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<T[]> data_;

    // Padded apart so producer and consumer do not share a cache line.
    alignas(64) std::atomic<std::size_t> read_{0};
    alignas(64) std::atomic<std::size_t> write_{0};
};

}  // namespace sonora
