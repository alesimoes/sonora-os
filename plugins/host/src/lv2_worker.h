// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// LV2 worker:schedule (http://lv2plug.in/ns/ext/worker). A plugin hands slow
// work (loading a sample, an IR, a preset file) from run() to a thread of the
// host's, and gets the answer back inside a later run(). The handoff in both
// directions is a lock-free ring, so run() never waits on the work and the
// work never waits on run(); the processing thread only bumps a word and
// wakes the worker, as it already wakes the engine (ipc.h signal).

#include "sonora/rt.h"

#include <lv2/core/lv2.h>
#include <lv2/worker/worker.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace sonora::plugin_host {

// Messages from one producer to one consumer: a 4-byte size, then its bytes,
// wrapping. No allocation after construction, no lock.
class WorkerRing {
public:
    // `capacity` is rounded up to a power of two.
    explicit WorkerRing(uint32_t capacity);

    // Producer. False when the message does not fit in what is free now.
    bool push(const void* data, uint32_t size) noexcept SONORA_RT;
    // Consumer. Where the producer has written to: a drain that stops there
    // is bounded even while the producer keeps writing.
    uint32_t mark() const noexcept SONORA_RT { return head_.load(std::memory_order_acquire); }
    // Consumer. The next message before `until` into `out` (at least
    // max_message() bytes). False when there is none.
    bool pop(uint32_t until, void* out, uint32_t& size) noexcept SONORA_RT;
    uint32_t max_message() const noexcept { return cap_ - kHeader; }

private:
    static constexpr uint32_t kHeader = sizeof(uint32_t);
    void write_at(uint32_t pos, const void* data, uint32_t n) noexcept SONORA_RT;
    void read_at(uint32_t pos, void* out, uint32_t n) const noexcept SONORA_RT;

    std::vector<unsigned char> bytes_;
    uint32_t cap_ = 0, mask_ = 0;
    // Free-running positions; head - tail is what is queued.
    std::atomic<uint32_t> head_{0};  // the producer's
    std::atomic<uint32_t> tail_{0};  // the consumer's
};

// The worker for one LV2 processor: a request and a response ring per
// instance (a mono plugin replicated per channel has one instance each), and
// one thread that calls work() for all of them in turn.
class Lv2Worker {
public:
    static constexpr uint32_t kRingBytes = 16 * 1024;

    Lv2Worker(uint32_t instances, uint32_t ring_bytes = kRingBytes);
    ~Lv2Worker();
    Lv2Worker(const Lv2Worker&) = delete;
    Lv2Worker& operator=(const Lv2Worker&) = delete;

    // The worker:schedule feature for instance `i`, to hand it at
    // instantiation. Address-stable for the worker's life.
    const LV2_Feature* feature(uint32_t i) const { return &queues_[i]->feature; }
    // After instantiation, before anything can schedule: the instance and its
    // worker:interface (null when it has none, and then its schedule_work
    // answers LV2_WORKER_ERR_UNKNOWN).
    void start(uint32_t i, LV2_Handle handle, const LV2_Worker_Interface* iface);
    // The thread, when any instance has an interface. False when the OS
    // refused it: the plugin must then be refused, not run without one.
    bool start_thread();
    // Joins the thread; what was still queued is dropped. Before the
    // instances are freed. A work() that has not returned within
    // kStopWaitMs is abandoned instead of waited on: false, and then the
    // instances, this worker and the plugin's library must outlive the
    // process (abandoned()), since that work() still runs inside them.
    static constexpr uint64_t kStopWaitMs = 1000;
    bool stop();

    // The processing thread, after instance `i`'s run(): its responses, then
    // its end_run(). Bounded by what had arrived when it began.
    void deliver(uint32_t i) noexcept SONORA_RT;

    // work() is in its own threading class, which LV2 forbids beside the
    // instantiation class (restore, activate, deactivate). The worker holds
    // the lock around every work() call; the main thread takes it around
    // those, waiting no later than `deadline_ns` (monotonic_ns): a work()
    // that never returns must not hold the main thread, which answers the
    // engine. Not owned past the deadline: the caller refuses what it was
    // about to do and says that work() did not return.
    std::unique_lock<std::timed_mutex> exclude_work(uint64_t deadline_ns);

    // A worker anywhere in the process was abandoned inside work(): the
    // plugins' libraries must not be unloaded (Lv2World).
    static bool abandoned() noexcept;

    // Requests and responses refused because their ring was full.
    uint64_t refused() const noexcept { return refused_.load(std::memory_order_relaxed); }

private:
    struct Queue {
        Lv2Worker* owner = nullptr;
        LV2_Worker_Schedule schedule{};
        LV2_Feature feature{};
        LV2_Handle handle = nullptr;
        const LV2_Worker_Interface* iface = nullptr;
        WorkerRing requests, responses;
        explicit Queue(uint32_t bytes) : requests(bytes), responses(bytes) {}
    };
    static LV2_Worker_Status schedule_work(LV2_Worker_Schedule_Handle h, uint32_t size,
                                           const void* data) noexcept SONORA_RT;
    static LV2_Worker_Status respond(LV2_Worker_Respond_Handle h, uint32_t size, const void* data) noexcept;
    void loop();

    std::vector<std::unique_ptr<Queue>> queues_;
    std::vector<unsigned char> run_scratch_, work_scratch_;  // one message each
    std::atomic<uint32_t> word_{0};
    std::atomic<uint32_t> exited_{0};  // 1 once loop() has returned
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> refused_{0};
    std::timed_mutex lock_;
    void* thread_ = nullptr;  // ipc.h start_thread
};

}  // namespace sonora::plugin_host
