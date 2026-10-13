// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plan retirement protocol (ADR-0009 §4.1) under stress. Meant to run
// under ThreadSanitizer as well as plainly: a fake callback thread runs
// blocks as fast as it can while the publisher swaps in a million plans,
// with deactivate/reactivate cycles in between. A plan freed while a block
// still holds it is a use-after-free TSan and ASan report; plainly, it shows
// as a plan whose contents change under the block reading it.
#include <atomic>
#include <cstdio>
#include <thread>

#include "sonora/plan_exchange.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using sonora::PlanExchange;
using sonora::PlaybackPlan;

constexpr uint32_t kSegments = 16;

// Every segment carries the epoch, so a block can tell whether the plan it
// holds was rewritten (freed and reused) while it read it.
PlaybackPlan* make_plan(uint64_t epoch) {
    sonora::PlanSizes sizes;
    sizes.segments = kSegments;
    PlaybackPlan* p = sonora::create_plan(sizes);
    p->epoch = epoch;
    for (uint32_t i = 0; i < kSegments; ++i) p->segments[i].start = static_cast<int64_t>(epoch);
    return p;
}

struct FakeCallback {
    PlanExchange& exchange;
    std::atomic<bool> run{true};
    std::atomic<bool> torn{false};
    std::atomic<uint64_t> swaps_seen{0};
    std::atomic<bool> epoch_went_back{false};
    uint64_t seen_epoch = 0;

    void block() {
        const PlaybackPlan* p = exchange.acquire();
        if (!p) return;
        if (p->epoch != seen_epoch) {
            if (p->epoch < seen_epoch) epoch_went_back.store(true);
            seen_epoch = p->epoch;
            swaps_seen.fetch_add(1, std::memory_order_relaxed);
        }
        const auto e = static_cast<int64_t>(p->epoch);
        // Twice, so a rewrite between the reads shows even if each read
        // alone looks consistent.
        for (int pass = 0; pass < 2; ++pass) {
            for (uint32_t i = 0; i < kSegments; ++i) {
                if (p->segments[i].start != e) torn.store(true);
            }
        }
    }

    void loop() {
        while (run.load(std::memory_order_relaxed)) block();
    }
};

int test_basic_protocol() {
    PlanExchange x;
    CHECK(x.acquire() == nullptr);
    PlaybackPlan* a = make_plan(x.next_epoch());
    CHECK(x.publish(a));
    CHECK(x.acquire() == a);
    PlaybackPlan* b = make_plan(x.next_epoch());
    CHECK(x.publish(b));
    // No block has started since the exchange: `a` may still be in use.
    CHECK(x.reclaim() == 1);
    CHECK(x.acquire() == b);
    CHECK(x.reclaim() == 0);
    CHECK(!x.is_retired(a));
    return 0;
}

int test_retire_list_is_bounded() {
    PlanExchange x;
    // The callback is stalled: nothing may be freed, and the publisher stops
    // at the list's capacity rather than growing it or freeing early.
    for (std::size_t i = 0; i <= PlanExchange::kRetireCapacity; ++i) {
        CHECK(x.publish(make_plan(x.next_epoch())));
    }
    CHECK(x.retired() == PlanExchange::kRetireCapacity);
    PlaybackPlan* extra = make_plan(x.next_epoch());
    CHECK(!x.publish(extra));
    CHECK(x.reclaim() == PlanExchange::kRetireCapacity);
    // deactivate() returned: everything retired is unreachable.
    CHECK(x.reclaim_after_deactivate() == 0);
    CHECK(x.publish(extra));
    return 0;
}

int test_may_free_holds_a_plan() {
    PlanExchange x;
    PlaybackPlan* a = make_plan(x.next_epoch());
    CHECK(x.publish(a));
    CHECK(x.publish(make_plan(x.next_epoch())));
    x.acquire();
    const auto hold = [](void*, const PlaybackPlan*) { return false; };
    CHECK(x.reclaim(hold) == 1);
    CHECK(x.reclaim_after_deactivate(hold) == 1);
    CHECK(x.is_retired(a));
    CHECK(x.reclaim() == 0);
    return 0;
}

// A million swaps against a free-running callback, in deactivate/reactivate
// cycles. Within a cycle the publisher frees only by the epoch rule; between
// cycles everything retired goes at once.
int test_stress() {
    constexpr uint64_t kSwaps = 1000000;
    constexpr int kCycles = 20;
    PlanExchange x;
    FakeCallback cb{x};
    uint64_t published = 0;
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        cb.run.store(true);
        std::thread audio([&cb] { cb.loop(); });
        const uint64_t target = kSwaps * static_cast<uint64_t>(cycle + 1) / kCycles;
        while (published < target) {
            PlaybackPlan* next = make_plan(x.next_epoch());
            while (!x.publish(next)) {
                x.reclaim();
                std::this_thread::yield();
            }
            ++published;
            x.reclaim();
        }
        cb.run.store(false);
        audio.join();  // deactivate(): no block runs or will start
        CHECK(x.reclaim_after_deactivate() == 0);
    }
    CHECK(!cb.torn.load());
    CHECK(!cb.epoch_went_back.load());
    CHECK(published == kSwaps);
    // A callback cannot see every swap, but it must see some in each cycle.
    CHECK(cb.swaps_seen.load() >= static_cast<uint64_t>(kCycles));
    std::printf("stress: %llu swaps published, %llu seen by the callback\n",
                static_cast<unsigned long long>(published),
                static_cast<unsigned long long>(cb.swaps_seen.load()));
    return 0;
}

}  // namespace

int main() {
    if (test_basic_protocol()) return 1;
    if (test_retire_list_is_bounded()) return 1;
    if (test_may_free_holds_a_plan()) return 1;
    if (test_stress()) return 1;
    std::printf("plan_exchange: ok\n");
    return 0;
}
