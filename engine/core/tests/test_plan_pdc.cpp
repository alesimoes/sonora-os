// SPDX-License-Identifier: GPL-3.0-or-later
//
// Insert chains and plugin delay compensation in the playback plan
// (ADR-0012 §7, checklist 3; plugin-latency-compensation.md): tracks whose
// chains declare different latencies, one of them 2048 frames, land
// sample-aligned at the bus; compensation past the session maximum is
// clamped and reported, not refused; the delay-line pool is capped and
// never indexed past (auditor L3); a latency change arrives as a new plan
// and the swap keeps every delay line's past; and Session::process()
// allocates nothing through any of it.
//
// The chains run on a test runner that behaves like a host with a declared
// latency: it returns its input latency_frames late, exactly. No host is
// involved; engine/plugin_client is the real runner.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

#include "sonora/insert_chain_runner.h"
#include "sonora/plan_graph.h"
#include "sonora/session.h"

// Every way process() could reach the heap is counted on the thread that
// calls it: each operator new and delete overload, and, outside the
// sanitizers (whose own interceptors must keep malloc), malloc, calloc,
// realloc, the aligned allocators and free.
namespace {
thread_local bool t_counting = false;
std::atomic<long> g_allocations{0};
std::atomic<long> g_frees{0};

void note_alloc() {
    if (t_counting) g_allocations.fetch_add(1, std::memory_order_relaxed);
}
void note_free(void* p) {
    if (t_counting && p != nullptr) g_frees.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace

#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define PDC_TEST_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define PDC_TEST_SANITIZED 1
#endif

#if defined(__GLIBC__) && !defined(PDC_TEST_SANITIZED)
#define PDC_TEST_COUNTS_MALLOC 1
extern "C" {
void* __libc_malloc(std::size_t);
void* __libc_calloc(std::size_t, std::size_t);
void* __libc_realloc(void*, std::size_t);
void* __libc_memalign(std::size_t, std::size_t);
void __libc_free(void*);

void* malloc(std::size_t n) {
    note_alloc();
    return __libc_malloc(n);
}
void* calloc(std::size_t n, std::size_t size) {
    note_alloc();
    return __libc_calloc(n, size);
}
void* realloc(void* p, std::size_t n) {
    note_alloc();
    return __libc_realloc(p, n);
}
void* aligned_alloc(std::size_t align, std::size_t n) {
    note_alloc();
    return __libc_memalign(align, n);
}
void* memalign(std::size_t align, std::size_t n) {
    note_alloc();
    return __libc_memalign(align, n);
}
int posix_memalign(void** out, std::size_t align, std::size_t n) {
    note_alloc();
    void* p = __libc_memalign(align, n);
    if (p == nullptr) return 12;  // ENOMEM
    *out = p;
    return 0;
}
void free(void* p) {
    note_free(p);
    __libc_free(p);
}
}
#endif

namespace {
void* counted_new(std::size_t size) {
    note_alloc();
    return std::malloc(size > 0 ? size : 1);
}
void* counted_new_aligned(std::size_t size, std::align_val_t align) {
    note_alloc();
    void* p = nullptr;
    const std::size_t a = static_cast<std::size_t>(align) < sizeof(void*)
                              ? sizeof(void*)
                              : static_cast<std::size_t>(align);
    return posix_memalign(&p, a, size > 0 ? size : 1) == 0 ? p : nullptr;
}
void counted_delete(void* p) {
    note_free(p);
    std::free(p);
}
}  // namespace

void* operator new(std::size_t size) {
    if (void* p = counted_new(size)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return counted_new(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return counted_new(size); }
void* operator new(std::size_t size, std::align_val_t a) {
    if (void* p = counted_new_aligned(size, a)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t a) { return operator new(size, a); }
void* operator new(std::size_t size, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_new_aligned(size, a);
}
void* operator new[](std::size_t size, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_new_aligned(size, a);
}
void operator delete(void* p) noexcept { counted_delete(p); }
void operator delete[](void* p) noexcept { counted_delete(p); }
void operator delete(void* p, std::size_t) noexcept { counted_delete(p); }
void operator delete[](void* p, std::size_t) noexcept { counted_delete(p); }
void operator delete(void* p, std::align_val_t) noexcept { counted_delete(p); }
void operator delete[](void* p, std::align_val_t) noexcept { counted_delete(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { counted_delete(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { counted_delete(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { counted_delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { counted_delete(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    counted_delete(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    counted_delete(p);
}

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using sonora::ChainBinding;
using sonora::DelayLinePool;
using sonora::NodeKind;
using sonora::PlaybackPlan;
using sonora::PlanNode;
using sonora::Session;

constexpr uint32_t kBlock = 64;
constexpr uint32_t kMaxCallback = 512;

void reset_heap_counts() {
    g_allocations.store(0);
    g_frees.store(0);
}
bool heap_untouched() { return g_allocations.load() == 0 && g_frees.load() == 0; }
// Frames of silence before the signal, so the faders' start-up ramp from 0
// never touches a sample that is checked.
constexpr int64_t kLeadIn = 4 * kBlock;

// Distinct per frame and exact in float, so a one-frame misalignment is a
// wrong value, and a sum of two is exact.
float signal(int64_t t) {
    if (t < kLeadIn) return 0.0f;
    return 1.0f + static_cast<float>(t % 4096) / 4096.0f;
}

// A host with a declared latency: one ring per channel slot, preallocated,
// returning each frame latency_frames late. The latency is read from the
// node each call, as a host's would change only with a new plan. It also
// checks the session's call sequence: begin, the chains, end, once each per
// callback, with the whole callback.
class DelayRunner final : public sonora::InsertChainRunner {
public:
    DelayRunner(uint32_t slots, uint32_t capacity)
        : cap_(capacity), ring_(static_cast<std::size_t>(slots) * capacity, 0.0f), pos_(slots, 0) {}

    void begin(uint64_t now_ns) noexcept SONORA_RT override {
        if (open_ || now_ns == 0) ++sequence_errors;
        open_ = true;
        ++begins;
    }

    void end() noexcept SONORA_RT override {
        if (!open_) ++sequence_errors;
        open_ = false;
        ++ends;
    }

    void run(const PlanNode& node, const float* const in[2], float* const out[2],
             uint32_t frames) noexcept SONORA_RT override {
        if (!open_ || frames != expected_frames) ++sequence_errors;
        float* ring = ring_.data() + static_cast<std::size_t>(node.channel_slot) * cap_;
        uint32_t pos = pos_[node.channel_slot];
        const uint32_t d = node.latency_frames;
        for (uint32_t i = 0; i < frames; ++i) {
            ring[pos] = in[0][i];
            out[0][i] = ring[(pos + cap_ - d) % cap_];
            out[1][i] = out[0][i];
            pos = (pos + 1) % cap_;
        }
        pos_[node.channel_slot] = pos;
        ++calls;
    }

    long begins = 0;
    long calls = 0;
    long ends = 0;
    long sequence_errors = 0;
    uint32_t expected_frames = kBlock;

private:
    bool open_ = false;
    uint32_t cap_;
    std::vector<float> ring_;
    std::vector<uint32_t> pos_;
};

struct Rig {
    std::unique_ptr<Session> session;
    std::vector<std::vector<float>> in;
    std::vector<const float*> in_ptrs;
    std::vector<float> out[2];
    float* out_ptrs[2];
    int64_t frame = 0;

    Rig(int tracks, uint32_t max_delay, uint32_t max_delayed_edges = 128,
        uint32_t max_block = 256) {
        sonora::SessionConfig cfg;
        cfg.dir = "test_plan_pdc_unused";
        cfg.track_count = tracks;
        cfg.ring_seconds = 0.1;
        cfg.playback.chunk_frames = 256;
        cfg.playback.window_seconds = 0.02;
        cfg.playback.pool_chunks = 64;
        cfg.playback.max_block_frames = max_block;
        cfg.playback.max_delay_frames = max_delay;
        cfg.playback.max_delayed_edges = max_delayed_edges;
        session = std::make_unique<Session>(cfg);
        in.assign(tracks, std::vector<float>(kMaxCallback, 0.0f));
        for (auto& v : in) in_ptrs.push_back(v.data());
        for (int o = 0; o < 2; ++o) {
            out[o].assign(kMaxCallback, 0.0f);
            out_ptrs[o] = out[o].data();
        }
        // Every track hears its input: the graph is under test, not the
        // disk path.
        for (auto& t : session->tracks()) t->set_monitor(sonora::Track::Monitor::Input);
    }

    // A plan with no regions over this session's tracks, chains as given,
    // compensated against the session's own pool and maximum.
    PlaybackPlan* plan(const std::vector<ChainBinding>& chains, DelayLinePool& pool,
                       sonora::Compensation* comp = nullptr,
                       std::vector<uint32_t>* shortfall = nullptr) {
        const auto tracks = static_cast<uint32_t>(session->tracks().size());
        uint32_t active = 0;
        for (const ChainBinding& c : chains) active += c.active ? 1 : 0;
        sonora::PlanSizes sizes;
        sizes.tracks = tracks;
        sizes.nodes = sonora::graph_nodes(tracks, active);
        sizes.edges = sonora::graph_edges(tracks, active);
        PlaybackPlan* p = sonora::create_plan(sizes);
        p->epoch = session->plans().next_epoch();
        p->chunk_frames = 256;
        p->pan_exponent = 0.5f;
        for (uint32_t t = 0; t < tracks; ++t) {
            p->tracks[t].engine_track = t;
            p->tracks[t].channels = 1;
        }
        sonora::layout_track_graph(*p, chains.data());
        if (shortfall) shortfall->assign(tracks, 0);
        const sonora::Compensation c = sonora::compensate(
            *p, session->max_delay_frames(), pool, shortfall ? shortfall->data() : nullptr);
        if (comp) *comp = c;
        return p;
    }

    bool publish(PlaybackPlan* p) {
        sonora::DiskReader& r = session->reader();
        const uint64_t gen = r.serve(session->plans().current(), p);
        r.run_pass();
        if (r.resident() != gen) return false;
        if (!session->plans().publish(p)) return false;
        r.serve(p, nullptr);
        r.run_pass();
        return true;
    }

    // One callback of signal on every input; counts any allocation or free
    // inside process().
    void block(uint32_t n = kBlock) {
        for (auto& v : in) {
            for (uint32_t i = 0; i < n; ++i) v[i] = signal(frame + i);
        }
        sonora::backend::AudioBlock b{};
        b.inputs = in_ptrs.data();
        b.input_count = static_cast<uint32_t>(in_ptrs.size());
        b.outputs = out_ptrs;
        b.output_count = 2;
        b.nframes = n;
        t_counting = true;
        session->process(b);
        t_counting = false;
        frame += n;
    }
};

ChainBinding chain(uint32_t slot, uint32_t latency) {
    ChainBinding c;
    c.active = true;
    c.chain_id = 100 + slot;
    c.channel_slot = slot;
    c.param_store = 7 + slot;
    c.latency_frames = latency;
    return c;
}

// The DAW 5 exit check: a track behind a 2048-frame chain, one behind a
// 100-frame chain and one with no chain, all on the same signal, land on
// the same frame at the bus.
int test_2048_lands_aligned() {
    Rig rig(3, 16384);
    DelayRunner runner(2, 4096);
    rig.session->set_insert_chains(&runner);
    // 0 and 2 left, 1 right: aligned, left is exactly twice right.
    rig.session->tracks()[0]->set_pan(-1.0f);
    rig.session->tracks()[1]->set_pan(1.0f);
    rig.session->tracks()[2]->set_pan(-1.0f);
    DelayLinePool pool(rig.session->delay_lines());
    sonora::Compensation comp;
    PlaybackPlan* p = rig.plan({chain(0, 2048), chain(1, 100), ChainBinding{}}, pool, &comp);
    CHECK(comp.compensation_frames == 2048);
    CHECK(comp.shortfall_frames == 0);
    CHECK(p->compensation_frames == 2048);
    // TrackSource, InsertChain, TrackFader twice, a TrackOut, the bus, out.
    CHECK(p->node_count == 9);
    CHECK(p->nodes[1].kind == NodeKind::InsertChain);
    CHECK(p->nodes[1].param_store == 7);   // A2.5: the store by index
    CHECK(p->nodes[4].kind == NodeKind::InsertChain);
    CHECK(p->last_insert_node == 4);
    // Into the bus: 0 for the slowest, 1948 behind the 100-frame chain,
    // 2048 for the dry track.
    const PlanNode& bus = p->nodes[7];
    CHECK(bus.kind == NodeKind::BusSum && bus.input_edge_count == 3);
    CHECK(p->edges[bus.first_input_edge].delay_frames == 0);
    CHECK(p->edges[bus.first_input_edge + 1].delay_frames == 1948);
    CHECK(p->edges[bus.first_input_edge + 2].delay_frames == 2048);
    CHECK(rig.publish(p));

    reset_heap_counts();
    long checked = 0;
    for (int blk = 0; blk < 80; ++blk) {
        const int64_t at = rig.frame;
        rig.block();
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float want = signal(at + i - 2048);
            CHECK(rig.out[1][i] == want);
            CHECK(rig.out[0][i] == want + want);
            if (want != 0.0f) ++checked;
        }
    }
    CHECK(checked > 2000);
    CHECK(runner.calls == 2 * 80);
    // begin, both chains, end: once each per callback, whole callback.
    CHECK(runner.begins == 80 && runner.ends == 80);
    CHECK(runner.sequence_errors == 0);
    CHECK(heap_untouched());
    return 0;
}

// Over the session maximum the plan is built and plays, the edge clamped to
// the maximum and the shortfall reported in frames, against the track.
int test_over_the_maximum_is_clamped_and_reported() {
    Rig rig(2, 1000);
    DelayRunner runner(1, 4096);
    rig.session->set_insert_chains(&runner);
    rig.session->tracks()[0]->set_pan(-1.0f);
    rig.session->tracks()[1]->set_pan(1.0f);
    DelayLinePool pool(rig.session->delay_lines());
    sonora::Compensation comp;
    std::vector<uint32_t> shortfall;
    PlaybackPlan* p = rig.plan({chain(0, 2048), ChainBinding{}}, pool, &comp, &shortfall);
    CHECK(comp.compensation_frames == 2048);
    CHECK(comp.clamped_edges == 1);
    CHECK(comp.shortfall_frames == 1048);
    CHECK(p->compensation_shortfall_frames == 1048);
    CHECK(shortfall[0] == 0);
    CHECK(shortfall[1] == 1048);
    CHECK(rig.publish(p));

    reset_heap_counts();
    for (int blk = 0; blk < 60; ++blk) {
        const int64_t at = rig.frame;
        rig.block();
        for (uint32_t i = 0; i < kBlock; ++i) {
            CHECK(rig.out[0][i] == signal(at + i - 2048));
            CHECK(rig.out[1][i] == signal(at + i - 1000));   // early, audibly: red
        }
    }
    CHECK(heap_untouched());
    return 0;
}

// Auditor L3: a plan needing more delayed edges than the pool holds is
// built with the excess undelayed and counted; no edge names a line past
// the pool, and the audio thread plays it.
int test_pool_cap() {
    Rig rig(3, 16384, 1);
    CHECK(rig.session->delay_lines() == 1);
    DelayRunner runner(1, 4096);
    rig.session->set_insert_chains(&runner);
    DelayLinePool pool(rig.session->delay_lines());
    sonora::Compensation comp;
    std::vector<uint32_t> shortfall;
    PlaybackPlan* p =
        rig.plan({chain(0, 512), ChainBinding{}, ChainBinding{}}, pool, &comp, &shortfall);
    CHECK(comp.undelayed_edges == 1);
    CHECK(p->undelayed_edges == 1);
    CHECK(comp.shortfall_frames == 512);
    CHECK(shortfall[1] == 0 && shortfall[2] == 512);
    uint32_t delayed = 0;
    for (uint32_t e = 0; e < p->edge_count; ++e) {
        if (p->edges[e].delay_frames == 0) continue;
        CHECK(p->edges[e].delay_line < rig.session->delay_lines());
        ++delayed;
    }
    CHECK(delayed == 1);
    rig.session->tracks()[0]->set_pan(-1.0f);
    rig.session->tracks()[1]->set_pan(1.0f);
    rig.session->tracks()[2]->set_pan(1.0f);
    CHECK(rig.publish(p));
    for (int blk = 0; blk < 20; ++blk) {
        const int64_t at = rig.frame;
        rig.block();
        for (uint32_t i = 0; i < kBlock; ++i) {
            CHECK(rig.out[0][i] == signal(at + i - 512));
            CHECK(rig.out[1][i] == signal(at + i - 512) + signal(at + i));
        }
    }
    return 0;
}

// The pool keeps a pair's line from build to build, hands a free line to a
// new pair before reusing one, and reuses the least recently used.
int test_delay_line_pool() {
    DelayLinePool pool(2);
    pool.begin_build();
    const uint32_t a = pool.assign(sonora::edge_pair(1, 9));
    const uint32_t b = pool.assign(sonora::edge_pair(2, 9));
    CHECK(a != b && a < 2 && b < 2);
    CHECK(pool.assign(sonora::edge_pair(3, 9)) == sonora::kNoDelayLine);
    pool.begin_build();
    CHECK(pool.assign(sonora::edge_pair(2, 9)) == b);
    CHECK(pool.assign(sonora::edge_pair(1, 9)) == a);
    pool.begin_build();
    CHECK(pool.assign(sonora::edge_pair(1, 9)) == a);
    // Pair 2 was not used in this build: its line is the one to go.
    CHECK(pool.assign(sonora::edge_pair(3, 9)) == b);
    DelayLinePool none(0);
    none.begin_build();
    CHECK(none.assign(sonora::edge_pair(1, 9)) == sonora::kNoDelayLine);
    return 0;
}

// A chain's latency changes: the control thread builds a new plan with the
// new figure, and the swap realigns at once. The dry track's edge keeps its
// delay line, so its longer delay is served from the past the line already
// holds, not from silence: channel state lives outside the plan.
int test_swap_with_latency_change() {
    Rig rig(2, 16384);
    DelayRunner runner(1, 4096);
    rig.session->set_insert_chains(&runner);
    rig.session->tracks()[0]->set_pan(-1.0f);
    rig.session->tracks()[1]->set_pan(1.0f);
    DelayLinePool pool(rig.session->delay_lines());
    PlaybackPlan* a = rig.plan({chain(0, 256), ChainBinding{}}, pool);
    const uint32_t line = a->edges[a->nodes[4].first_input_edge + 1].delay_line;
    CHECK(rig.publish(a));

    reset_heap_counts();
    for (int blk = 0; blk < 30; ++blk) {
        const int64_t at = rig.frame;
        rig.block();
        for (uint32_t i = 0; i < kBlock; ++i) {
            CHECK(rig.out[0][i] == signal(at + i - 256));
            CHECK(rig.out[1][i] == signal(at + i - 256));
        }
    }

    PlaybackPlan* b = rig.plan({chain(0, 512), ChainBinding{}}, pool);
    CHECK(b->compensation_frames == 512);
    const sonora::PlanEdge& dry = b->edges[b->nodes[4].first_input_edge + 1];
    CHECK(dry.delay_frames == 512);
    CHECK(dry.delay_line == line);
    CHECK(rig.publish(b));
    for (int blk = 0; blk < 30; ++blk) {
        const int64_t at = rig.frame;
        rig.block();
        for (uint32_t i = 0; i < kBlock; ++i) {
            CHECK(rig.out[0][i] == signal(at + i - 512));
            CHECK(rig.out[1][i] == signal(at + i - 512));
        }
    }
    CHECK(heap_untouched());
    return 0;
}

// No runner installed, or a chain bound to no slot: the chain passes its
// input through, and a track with a chain sounds as it would without one.
int test_unbound_chain_passes_through() {
    Rig rig(2, 16384);
    rig.session->tracks()[0]->set_pan(-1.0f);
    rig.session->tracks()[1]->set_pan(1.0f);
    DelayLinePool pool(rig.session->delay_lines());
    ChainBinding unbound;
    unbound.active = true;
    CHECK(rig.publish(rig.plan({unbound, ChainBinding{}}, pool)));
    reset_heap_counts();
    for (int blk = 0; blk < 10; ++blk) {
        const int64_t at = rig.frame;
        rig.block();
        for (uint32_t i = 0; i < kBlock; ++i) {
            CHECK(rig.out[0][i] == signal(at + i));
            CHECK(rig.out[1][i] == signal(at + i));
        }
    }
    CHECK(heap_untouched());
    return 0;
}

// Review of PR #48 (H1): an edge delayed, then undelayed for a while, then
// delayed again gets its old line back from the pool. That line holds the
// audio from before the gap; replaying it would put tens of milliseconds
// of the past on the bus. It must restart from silence instead.
int test_line_restarts_after_a_gap() {
    Rig rig(2, 16384);
    DelayRunner runner(1, 4096);
    rig.session->set_insert_chains(&runner);
    rig.session->tracks()[0]->set_pan(-1.0f);
    rig.session->tracks()[1]->set_pan(1.0f);
    DelayLinePool pool(rig.session->delay_lines());
    PlaybackPlan* a = rig.plan({chain(0, 512), ChainBinding{}}, pool);
    const uint32_t line = a->edges[a->nodes[4].first_input_edge + 1].delay_line;
    CHECK(line != sonora::kNoDelayLine);
    CHECK(rig.publish(a));
    for (int blk = 0; blk < 20; ++blk) rig.block();

    // The insert goes: nothing is delayed, the line is not written.
    CHECK(rig.publish(rig.plan({ChainBinding{}, ChainBinding{}}, pool)));
    for (int blk = 0; blk < 20; ++blk) {
        const int64_t at = rig.frame;
        rig.block();
        for (uint32_t i = 0; i < kBlock; ++i) CHECK(rig.out[1][i] == signal(at + i));
    }

    // It comes back: the same line, which must not replay the old past.
    PlaybackPlan* c = rig.plan({chain(0, 512), ChainBinding{}}, pool);
    CHECK(c->edges[c->nodes[4].first_input_edge + 1].delay_line == line);
    CHECK(rig.publish(c));
    const int64_t swap = rig.frame;
    reset_heap_counts();
    for (int blk = 0; blk < 20; ++blk) {
        const int64_t at = rig.frame;
        rig.block();
        for (uint32_t i = 0; i < kBlock; ++i) {
            const int64_t t = at + i;
            CHECK(rig.out[1][i] == (t < swap + 512 ? 0.0f : signal(t - 512)));
        }
    }
    CHECK(heap_untouched());
    return 0;
}

// Review of PR #48 (M1): stable ids follow the track, not its position.
// Tracks A and B dry, C behind a 512-frame chain; A is deleted, so B moves
// to position 0. B's edge into the bus must keep B's delay line, never
// take A's, whose past is another track's audio.
int test_stable_ids_follow_the_track() {
    Rig rig(3, 16384);
    DelayLinePool pool(rig.session->delay_lines());
    ChainBinding a;
    a.stable_key = 10;
    ChainBinding b;
    b.stable_key = 11;
    ChainBinding c = chain(0, 512);
    c.stable_key = 12;
    PlaybackPlan* before = rig.plan({a, b, c}, pool);
    // A TrackOut, B TrackOut, C's three, bus: A's and B's edges into it.
    const PlanNode& bus1 = before->nodes[5];
    CHECK(bus1.kind == NodeKind::BusSum);
    CHECK(before->nodes[0].stable_id == 10 && before->nodes[1].stable_id == 11);
    const uint32_t line_a = before->edges[bus1.first_input_edge].delay_line;
    const uint32_t line_b = before->edges[bus1.first_input_edge + 1].delay_line;
    CHECK(line_a != sonora::kNoDelayLine && line_b != sonora::kNoDelayLine && line_a != line_b);
    CHECK(rig.publish(before));

    ChainBinding spare;
    spare.stable_key = sonora::kSpareTrackKeyTag | 3;
    PlaybackPlan* after = rig.plan({b, c, spare}, pool);
    CHECK(after->nodes[0].stable_id == 11);
    CHECK(after->nodes[2].stable_id == (sonora::kInsertChainIdTag | 12));
    const PlanNode& bus2 = after->nodes[5];
    CHECK(bus2.kind == NodeKind::BusSum);
    CHECK(after->edges[bus2.first_input_edge].delay_line == line_b);
    CHECK(after->edges[bus2.first_input_edge + 2].delay_line != line_b);
    CHECK(rig.publish(after));
    return 0;
}

// A callback longer than max_block_frames is mixed in pieces; the depth-1
// protocol needs one call per callback with the whole of it, so the chains
// pass through unrun, counted, and the runner sees neither begin nor end.
int test_long_callback_bypasses_chains() {
    Rig rig(2, 16384, 128, kBlock);
    DelayRunner runner(1, 4096);
    rig.session->set_insert_chains(&runner);
    rig.session->tracks()[0]->set_pan(-1.0f);
    rig.session->tracks()[1]->set_pan(1.0f);
    DelayLinePool pool(rig.session->delay_lines());
    CHECK(rig.publish(rig.plan({chain(0, 0), ChainBinding{}}, pool)));
    for (int blk = 0; blk < 8; ++blk) rig.block();
    CHECK(runner.calls == 8 && runner.begins == 8 && runner.ends == 8);

    reset_heap_counts();
    const int64_t at = rig.frame;
    rig.block(2 * kBlock);
    CHECK(heap_untouched());
    CHECK(runner.calls == 8 && runner.begins == 8 && runner.ends == 8);
    CHECK(rig.session->chain_bypassed_blocks() == 1);
    for (uint32_t i = 0; i < 2 * kBlock; ++i) {
        CHECK(rig.out[0][i] == signal(at + i));
        CHECK(rig.out[1][i] == signal(at + i));
    }
    rig.block();
    CHECK(runner.calls == 9 && runner.begins == 9 && runner.ends == 9);
    CHECK(runner.sequence_errors == 0);
    return 0;
}

// A plan whose indices leave the session's buffers is played as silence and
// counted, never followed (rt audit of PR #48: a bound checked, not only
// asserted). The next valid plan plays.
int test_out_of_bounds_plan_is_silence() {
    Rig rig(1, 0);
    rig.session->tracks()[0]->set_pan(-1.0f);
    DelayLinePool pool(rig.session->delay_lines());
    PlaybackPlan* bad = rig.plan({ChainBinding{}}, pool);
    bad->nodes[0].scratch_buffer = 9999;
    CHECK(rig.publish(bad));
    reset_heap_counts();
    rig.block();
    CHECK(heap_untouched());
    CHECK(rig.session->invalid_plan_blocks() == 1);
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(rig.out[0][i] == 0.0f && rig.out[1][i] == 0.0f);
    CHECK(rig.publish(rig.plan({ChainBinding{}}, pool)));
    for (int blk = 0; blk < 6; ++blk) rig.block();
    const int64_t at = rig.frame;
    rig.block();
    CHECK(rig.session->invalid_plan_blocks() == 1);
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(rig.out[0][i] == signal(at + i));
    return 0;
}

}  // namespace

int main() {
#if defined(PDC_TEST_COUNTS_MALLOC)
    std::printf("plan_pdc: counting malloc, calloc, realloc, aligned and free too\n");
#endif
    if (test_2048_lands_aligned()) return 1;
    if (test_over_the_maximum_is_clamped_and_reported()) return 1;
    if (test_pool_cap()) return 1;
    if (test_delay_line_pool()) return 1;
    if (test_swap_with_latency_change()) return 1;
    if (test_unbound_chain_passes_through()) return 1;
    if (test_line_restarts_after_a_gap()) return 1;
    if (test_stable_ids_follow_the_track()) return 1;
    if (test_long_callback_bypasses_chains()) return 1;
    if (test_out_of_bounds_plan_is_silence()) return 1;
    std::printf("plan_pdc: ok\n");
    return 0;
}
