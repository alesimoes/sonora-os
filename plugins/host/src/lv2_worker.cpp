// SPDX-License-Identifier: GPL-3.0-or-later
#include "lv2_worker.h"

#include "ipc.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace sonora::plugin_host {

namespace {

uint32_t round_up_pow2(uint32_t v) {
    uint32_t p = 64;
    while (p < v) p <<= 1;
    return p;
}

// The worker sleeps at most this long between looks, so a wake lost to a
// race costs one interval, never a stuck request.
constexpr uint64_t kWorkerIdleNs = 100'000'000;
// Under lock_memory a thread's whole stack is resident (ipc.h start_thread),
// and the default is 8 MiB. Half the processing thread's: work() is the
// plugin's file loading, which keeps its buffers on the heap.
constexpr std::size_t kWorkerStack = std::size_t(512) << 10;

std::atomic<bool> g_abandoned{false};

}  // namespace

// ---------------------------------------------------------------------------
// WorkerRing

WorkerRing::WorkerRing(uint32_t capacity)
    : bytes_(round_up_pow2(capacity)), cap_(static_cast<uint32_t>(bytes_.size())), mask_(cap_ - 1) {}

void WorkerRing::write_at(uint32_t pos, const void* data, uint32_t n) noexcept SONORA_RT {
    const uint32_t at = pos & mask_;
    const uint32_t first = std::min(n, cap_ - at);
    std::memcpy(bytes_.data() + at, data, first);
    std::memcpy(bytes_.data(), static_cast<const unsigned char*>(data) + first, n - first);
}

void WorkerRing::read_at(uint32_t pos, void* out, uint32_t n) const noexcept SONORA_RT {
    const uint32_t at = pos & mask_;
    const uint32_t first = std::min(n, cap_ - at);
    std::memcpy(out, bytes_.data() + at, first);
    std::memcpy(static_cast<unsigned char*>(out) + first, bytes_.data(), n - first);
}

bool WorkerRing::push(const void* data, uint32_t size) noexcept SONORA_RT {
    if (size > max_message()) return false;
    const uint32_t head = head_.load(std::memory_order_relaxed);
    const uint32_t tail = tail_.load(std::memory_order_acquire);
    if (cap_ - (head - tail) < kHeader + size) return false;
    write_at(head, &size, kHeader);
    if (size) write_at(head + kHeader, data, size);
    head_.store(head + kHeader + size, std::memory_order_release);
    return true;
}

bool WorkerRing::pop(uint32_t until, void* out, uint32_t& size) noexcept SONORA_RT {
    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == until) return false;
    read_at(tail, &size, kHeader);
    if (size) read_at(tail + kHeader, out, size);
    tail_.store(tail + kHeader + size, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------
// Lv2Worker

Lv2Worker::Lv2Worker(uint32_t instances, uint32_t ring_bytes) {
    queues_.reserve(instances);
    for (uint32_t i = 0; i < instances; ++i) {
        auto q = std::make_unique<Queue>(ring_bytes);
        q->owner = this;
        q->schedule = {q.get(), &Lv2Worker::schedule_work};
        q->feature = {LV2_WORKER__schedule, &q->schedule};
        queues_.push_back(std::move(q));
    }
    const uint32_t message = queues_.empty() ? 0 : queues_[0]->requests.max_message();
    run_scratch_.resize(message);
    work_scratch_.resize(message);
}

Lv2Worker::~Lv2Worker() { stop(); }

void Lv2Worker::start(uint32_t i, LV2_Handle handle, const LV2_Worker_Interface* iface) {
    queues_[i]->handle = handle;
    queues_[i]->iface = iface && iface->work && iface->work_response ? iface : nullptr;
}

bool Lv2Worker::start_thread() {
    if (thread_) return true;
    for (const auto& q : queues_)
        if (q->iface) {
            thread_ = plugin_host::start_thread([](void* w) { static_cast<Lv2Worker*>(w)->loop(); }, this,
                                                kWorkerStack);
            return thread_ != nullptr;
        }
    return true;
}

bool Lv2Worker::stop() {
    if (!thread_) return true;
    stop_.store(true, std::memory_order_release);
    signal(word_);
    // The loop looks at stop_ before every work(), so only a work() that is
    // itself stuck keeps it past this.
    const uint64_t end = monotonic_ns() + kStopWaitMs * 1'000'000ull;
    for (uint64_t now = monotonic_ns(); exited_.load(std::memory_order_acquire) == 0 && now < end;
         now = monotonic_ns())
        wait_for_change(exited_, 0, end - now);
    if (exited_.load(std::memory_order_acquire) == 0) {
        g_abandoned.store(true, std::memory_order_release);
        return false;
    }
    join_thread(thread_);
    thread_ = nullptr;
    return true;
}

bool Lv2Worker::abandoned() noexcept { return g_abandoned.load(std::memory_order_acquire); }

std::unique_lock<std::timed_mutex> Lv2Worker::exclude_work(uint64_t deadline_ns) {
    std::unique_lock<std::timed_mutex> g(lock_, std::defer_lock);
    const uint64_t now = monotonic_ns();
    // try_lock_for, not try_lock_until: monotonic_ns is CLOCK_MONOTONIC, and
    // only a duration needs no agreement on the clock.
    (void)g.try_lock_for(std::chrono::nanoseconds(deadline_ns > now ? deadline_ns - now : 0));
    return g;
}

// From run(), or from restore() when the plugin was handed the feature there;
// never both at once, since restore() never runs beside run(), so each
// request ring keeps its single producer.
LV2_Worker_Status Lv2Worker::schedule_work(LV2_Worker_Schedule_Handle h, uint32_t size,
                                           const void* data) noexcept SONORA_RT {
    auto* q = static_cast<Queue*>(h);
    if (!q->iface) return LV2_WORKER_ERR_UNKNOWN;
    if (!q->requests.push(data, size)) {
        q->owner->refused_.fetch_add(1, std::memory_order_relaxed);
        return LV2_WORKER_ERR_NO_SPACE;
    }
    signal(q->owner->word_);
    return LV2_WORKER_SUCCESS;
}

// The worker thread, inside work().
LV2_Worker_Status Lv2Worker::respond(LV2_Worker_Respond_Handle h, uint32_t size, const void* data) noexcept {
    auto* q = static_cast<Queue*>(h);
    if (!q->responses.push(data, size)) {
        q->owner->refused_.fetch_add(1, std::memory_order_relaxed);
        return LV2_WORKER_ERR_NO_SPACE;
    }
    return LV2_WORKER_SUCCESS;
}

void Lv2Worker::deliver(uint32_t i) noexcept SONORA_RT {
    Queue& q = *queues_[i];
    if (!q.iface) return;
    const uint32_t until = q.responses.mark();
    uint32_t size = 0;
    while (q.responses.pop(until, run_scratch_.data(), size))
        q.iface->work_response(q.handle, size, run_scratch_.data());
    if (q.iface->end_run) q.iface->end_run(q.handle);
}

void Lv2Worker::loop() {
    while (!stop_.load(std::memory_order_acquire)) {
        // Read before draining: a request pushed after the drain has moved
        // the word, so the wait below returns at once.
        const uint32_t seen = word_.load(std::memory_order_acquire);
        for (const auto& q : queues_) {
            if (!q->iface) continue;
            const uint32_t until = q->requests.mark();
            uint32_t size = 0;
            while (!stop_.load(std::memory_order_acquire) &&
                   q->requests.pop(until, work_scratch_.data(), size)) {
                const std::lock_guard<std::timed_mutex> g(lock_);
                q->iface->work(q->handle, &Lv2Worker::respond, q.get(), size, work_scratch_.data());
            }
        }
        if (stop_.load(std::memory_order_acquire)) break;
        wait_for_change(word_, seen, kWorkerIdleNs);
    }
    signal(exited_);
}

}  // namespace sonora::plugin_host
