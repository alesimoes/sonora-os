// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/graph_workers.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

namespace sonora {
namespace {

// The cursor is one atomic {epoch, index}, so a claim made against a level
// that is already settled cannot land in the next one.
constexpr unsigned kIndexBits = 24;
constexpr uint64_t kIndexMask = (uint64_t{1} << kIndexBits) - 1;
constexpr uint64_t kEpochMask = (uint64_t{1} << 40) - 1;

// A node's state is {epoch, phase}. Only the audio thread moves a node into
// Ready or out of Ready/Running at the deadline; a claimer moves it from
// Ready to Running and Running to Done. Each move is a compare-exchange on
// the whole word, so the epoch decides which side wins.
enum Phase : uint64_t {
    kIdle = 0,
    kReady = 1,
    kRunning = 2,
    kDone = 3,
    kAbandoned = 4,  // dropped while a worker is still inside run()
};
constexpr unsigned kPhaseBits = 3;

constexpr uint64_t pack_state(uint64_t epoch, Phase phase) noexcept {
    return (epoch << kPhaseBits) | phase;
}
constexpr uint64_t phase_of(uint64_t state) noexcept {
    return state & ((uint64_t{1} << kPhaseBits) - 1);
}

// The deadline poll reads the clock once per this many pauses.
constexpr uint32_t kPausesPerClockRead = 16;

inline void cpu_pause() noexcept SONORA_RT {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#endif
}

class MonotonicClock final : public GraphClock {
public:
    uint64_t now_ns() const noexcept SONORA_RT override {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    }
};

class RtPrioritySetup final : public GraphWorkerSetup {
public:
    explicit RtPrioritySetup(int priority) : priority_(priority) {}

    const char* prepare(uint32_t) noexcept override {
        if (priority_ <= 0) return nullptr;
#if defined(__linux__)
        sched_param param{};
        param.sched_priority = priority_;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
            return "could not set its SCHED_FIFO priority";
        }
        return nullptr;
#else
        return "real-time priority is not supported on this platform";
#endif
    }

private:
    const int priority_;
};

class WorkerPool final : public GraphWorkerPool {
public:
    WorkerPool(const GraphSchedulerConfig& config, const GraphWorkerOptions& options)
        : capacity_(config.max_nodes),
          book_(config.max_nodes),
          state_(std::make_unique<std::atomic<uint64_t>[]>(config.max_nodes + 1u)),
          list_(std::make_unique<std::atomic<uint32_t>[]>(config.max_nodes + 1u)),
          clock_(options.clock != nullptr ? options.clock : &monotonic_graph_clock()),
          default_setup_(options.rt_priority),
          setup_(options.setup != nullptr ? options.setup : &default_setup_),
          idle_spins_(options.idle_spins),
          idle_sleep_ns_(options.idle_sleep_ns) {}

    ~WorkerPool() override { stop(); }

    // Control thread. Empty on success.
    std::string start(uint32_t workers) {
        why_ = std::make_unique<std::atomic<const char*>[]>(workers);
        threads_.reserve(workers);
        for (uint32_t i = 0; i < workers; ++i) {
            try {
                threads_.emplace_back([this, i] { worker_main(i); });
            } catch (const std::system_error& e) {
                stop();
                return "graph worker " + std::to_string(i) + ": thread not created: " + e.what();
            }
        }
        // Every worker reports once, right after its setup.
        while (reported_.load(std::memory_order_acquire) < workers) std::this_thread::yield();
        for (uint32_t i = 0; i < workers; ++i) {
            if (const char* why = why_[i].load(std::memory_order_acquire)) {
                stop();
                return "graph worker " + std::to_string(i) + ": " + why;
            }
        }
        started_.store(true, std::memory_order_release);
        return {};
    }

    uint32_t workers() const noexcept override { return static_cast<uint32_t>(threads_.size()); }

    GraphLevelResult run_level(const GraphLevel& level,
                               GraphNodeWork& work) noexcept SONORA_RT override {
        GraphLevelResult r;
        if (level.nodes == nullptr || level.count == 0) return r;
        epoch_ = (epoch_ + 1) & kEpochMask;
        if (epoch_ == 0) epoch_ = 1;
        const uint64_t epoch = epoch_;
        const uint32_t offered = offer(level, work, epoch, r);
        if (offered == 0) return r;
        help(work, epoch, offered);
        await(level.deadline_ns, epoch, offered);
        settle(work, epoch, offered, r);
        return r;
    }

    void reset_drops() noexcept SONORA_RT override { book_.reset(); }

    GraphDropCounts drop_counts() const noexcept override { return book_.counts(); }

    uint64_t late_results() const noexcept override {
        return late_.load(std::memory_order_acquire);
    }

private:
    // Down nodes and nodes whose worker has not come back are dropped here,
    // before anyone can claim them; the rest are marked ready and listed.
    uint32_t offer(const GraphLevel& level, GraphNodeWork& work, uint64_t epoch,
                   GraphLevelResult& r) noexcept SONORA_RT {
        uint32_t offered = 0;
        for (uint32_t i = 0; i < level.count; ++i) {
            const uint32_t n = level.nodes[i];
            if (n >= capacity_) {
                // No state slot: the session sized us too small. Serial is
                // still correct, so run it here rather than lose it.
                work.run(n);
                ++r.ran;
                continue;
            }
            if (book_.down(n)) {
                work.drop(n);
                book_.note_skipped(n);
                ++r.skipped;
                continue;
            }
            const uint64_t phase = phase_of(state_[n].load(std::memory_order_acquire));
            if (phase == kAbandoned || phase == kRunning) {
                work.drop(n);
                book_.note_dropped(n);
                ++r.dropped;
                continue;
            }
            state_[n].store(pack_state(epoch, kReady), std::memory_order_relaxed);
            list_[offered++].store(n, std::memory_order_relaxed);
        }
        if (offered == 0) return 0;
        work_.store(&work, std::memory_order_release);
        count_.store(offered, std::memory_order_release);
        // Publishes the ready states and the list above.
        cursor_.store(epoch << kIndexBits, std::memory_order_release);
        return offered;
    }

    // Help first: the audio thread claims like any worker until the cursor
    // is exhausted.
    void help(GraphNodeWork& work, uint64_t epoch, uint32_t offered) noexcept SONORA_RT {
        uint64_t c = cursor_.load(std::memory_order_acquire);
        while ((c & kIndexMask) < offered) {
            if (!cursor_.compare_exchange_weak(c, c + 1, std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                continue;
            }
            const uint32_t n = list_[c & kIndexMask].load(std::memory_order_relaxed);
            uint64_t expect = pack_state(epoch, kReady);
            if (state_[n].compare_exchange_strong(expect, pack_state(epoch, kRunning),
                                                  std::memory_order_acq_rel)) {
                work.run(n);
                // Only the audio thread abandons, so nothing raced this.
                state_[n].store(pack_state(epoch, kDone), std::memory_order_release);
            }
            c = cursor_.load(std::memory_order_acquire);
        }
    }

    // Bounded spin: the clock is read only while a worker still holds a node.
    void await(uint64_t deadline_ns, uint64_t epoch, uint32_t offered) noexcept SONORA_RT {
        const uint64_t done = pack_state(epoch, kDone);
        uint32_t k = 0;
        uint32_t pauses = 0;
        while (k < offered) {
            const uint32_t n = list_[k].load(std::memory_order_relaxed);
            if (state_[n].load(std::memory_order_acquire) == done) {
                ++k;
                continue;
            }
            if (pauses++ % kPausesPerClockRead == 0 && clock_->now_ns() >= deadline_ns) return;
            cpu_pause();
        }
    }

    void settle(GraphNodeWork& work, uint64_t epoch, uint32_t offered,
                GraphLevelResult& r) noexcept SONORA_RT {
        for (uint32_t i = 0; i < offered; ++i) {
            const uint32_t n = list_[i].load(std::memory_order_relaxed);
            if (take_back(n, epoch)) {
                book_.note_ran(n);
                ++r.ran;
            } else {
                work.drop(n);
                book_.note_dropped(n);
                ++r.dropped;
            }
        }
    }

    // True when the node finished for this epoch. Otherwise it is now Idle
    // (never started: no claimer can start it any more) or Abandoned (its
    // worker's result will be discarded).
    bool take_back(uint32_t n, uint64_t epoch) noexcept SONORA_RT {
        uint64_t s = state_[n].load(std::memory_order_acquire);
        for (;;) {
            if (s == pack_state(epoch, kDone)) return true;
            Phase next;
            if (s == pack_state(epoch, kReady)) {
                next = kIdle;
            } else if (s == pack_state(epoch, kRunning)) {
                next = kAbandoned;
            } else {
                return false;
            }
            if (state_[n].compare_exchange_weak(s, pack_state(next == kIdle ? 0 : epoch, next),
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
                return false;
            }
        }
    }

    // One claim attempt. False when there is nothing to claim.
    bool worker_step() noexcept SONORA_RT {
        uint64_t c = cursor_.load(std::memory_order_acquire);
        if ((c & kIndexMask) >= count_.load(std::memory_order_acquire)) return false;
        if (!cursor_.compare_exchange_weak(c, c + 1, std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            return true;
        }
        const uint64_t epoch = c >> kIndexBits;
        const uint32_t n = list_[c & kIndexMask].load(std::memory_order_relaxed);
        // Read before the claim: once the node is Ready for `epoch`, the
        // audio thread has not moved on to another level, so this is that
        // level's work.
        GraphNodeWork* const work = work_.load(std::memory_order_acquire);
        if (n >= capacity_ || work == nullptr) return true;
        uint64_t expect = pack_state(epoch, kReady);
        if (!state_[n].compare_exchange_strong(expect, pack_state(epoch, kRunning),
                                               std::memory_order_acq_rel)) {
            return true;  // settled without us, or a stale claim
        }
        work->run(n);
        expect = pack_state(epoch, kRunning);
        if (!state_[n].compare_exchange_strong(expect, pack_state(epoch, kDone),
                                               std::memory_order_acq_rel)) {
            // Abandoned at the deadline: the result is late and nobody reads
            // it; Idle lets the audio thread offer the node again.
            state_[n].store(pack_state(0, kIdle), std::memory_order_release);
            // Release: whoever sees the count also sees the node Idle.
            late_.fetch_add(1, std::memory_order_release);
        }
        return true;
    }

    void worker_main(uint32_t index) {
        why_[index].store(setup_->prepare(index), std::memory_order_release);
        reported_.fetch_add(1, std::memory_order_acq_rel);
        while (!started_.load(std::memory_order_acquire)) {
            if (stop_.load(std::memory_order_acquire)) return;
            std::this_thread::yield();
        }
        uint32_t idle = 0;
        while (!stop_.load(std::memory_order_acquire)) {
            if (worker_step()) {
                idle = 0;
                continue;
            }
            if (idle < idle_spins_) {
                ++idle;
                cpu_pause();
            } else {
                std::this_thread::sleep_for(std::chrono::nanoseconds(idle_sleep_ns_));
            }
        }
    }

    void stop() {
        stop_.store(true, std::memory_order_release);
        for (std::thread& t : threads_) {
            if (t.joinable()) t.join();
        }
        threads_.clear();
    }

    const uint32_t capacity_;
    GraphDropBook book_;
    std::unique_ptr<std::atomic<uint64_t>[]> state_;
    std::unique_ptr<std::atomic<uint32_t>[]> list_;
    const GraphClock* const clock_;
    RtPrioritySetup default_setup_;
    GraphWorkerSetup* const setup_;
    const uint32_t idle_spins_;
    const uint32_t idle_sleep_ns_;

    // The audio thread's alone.
    uint64_t epoch_ = 0;

    // Shared with the workers; the cursor apart from the rest, since every
    // claim writes it.
    alignas(64) std::atomic<uint64_t> cursor_{0};
    alignas(64) std::atomic<uint32_t> count_{0};
    std::atomic<GraphNodeWork*> work_{nullptr};
    std::atomic<uint64_t> late_{0};
    std::atomic<bool> stop_{false};
    std::atomic<bool> started_{false};

    // Start-up only.
    std::unique_ptr<std::atomic<const char*>[]> why_;
    std::atomic<uint32_t> reported_{0};
    std::vector<std::thread> threads_;
};

}  // namespace

const GraphClock& monotonic_graph_clock() noexcept {
    static const MonotonicClock clock;
    return clock;
}

GraphWorkersStarted start_graph_workers(const GraphSchedulerConfig& config,
                                        const GraphWorkerOptions& options) {
    GraphWorkersStarted out;
    if (config.workers == 0) {
        out.error = "graph workers: 0 workers is the serial scheduler";
        return out;
    }
    if (config.max_nodes > kIndexMask) {
        out.error = "graph workers: " + std::to_string(config.max_nodes) +
                    " nodes is more than a level cursor holds";
        return out;
    }
    auto pool = std::make_unique<WorkerPool>(config, options);
    out.error = pool->start(config.workers);
    if (out.error.empty()) out.pool = std::move(pool);
    return out;
}

}  // namespace sonora
