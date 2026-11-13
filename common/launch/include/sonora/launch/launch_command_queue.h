#pragma once

// Single-producer single-consumer queue of fixed-size POD launcher commands
// (ADR-0164 section 2). Storage is allocated once at construction; push and
// pop never allocate, lock or wait. A full queue refuses the push and the
// caller counts it.
//
// Its own small ring rather than common/base's RingBuffer: this module stays
// free of module edges (ADR-0007), and a command queue moves whole structs.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

#include "sonora/launch/launch_grid.h"
#include "sonora/launch/launch_quantize.h"

namespace sonora::launch {

struct Command {
    enum class Kind : std::uint8_t { AssignClip, SetQuantize, TriggerCell, TriggerScene, StopTrack, StopAll };
    Kind kind = Kind::StopAll;
    Quantize quantize = Quantize::Bar;
    std::uint32_t track = 0;
    std::uint32_t scene = 0;
    ClipId clip = kNoClip;
};
static_assert(std::is_trivially_copyable<Command>::value, "commands cross threads by copy");

class CommandQueue {
public:
    // capacity is rounded up to a power of two (at least 2).
    explicit CommandQueue(std::size_t capacity)
        : capacity_(round_up(capacity)), mask_(capacity_ - 1),
          slots_(new Command[capacity_]) {}

    std::size_t capacity() const { return capacity_; }

    bool push(const Command& c) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail == capacity_) return false;
        slots_[head & mask_] = c;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool pop(Command& out) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (head == tail) return false;
        out = slots_[tail & mask_];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

private:
    static std::size_t round_up(std::size_t n) {
        std::size_t p = 2;
        while (p < n) p <<= 1;
        return p;
    }

    const std::size_t capacity_;
    const std::size_t mask_;
    std::unique_ptr<Command[]> slots_;
    // Separate cache lines: producer and consumer each write one index.
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

}  // namespace sonora::launch
