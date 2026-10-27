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
#include <cstdio>
#include <memory>
#include <vector>

// The heap counter (playback_test.h): operator new/delete and, outside the
// sanitizers, malloc and free, on the thread inside process().
#define PLAYBACK_TEST_COUNT_HEAP 1
#include "playback_test.h"
#include "sonora/insert_chain_runner.h"
#include "sonora/plan_graph.h"
#include "sonora/plugin_chain_runner.h"
#include "sonora/session.h"

namespace {

using playback_test::heap_untouched;
using playback_test::reset_heap_counts;
using playback_test::t_counting;
using sonora::ChainBinding;
using sonora::DelayLinePool;
using sonora::NodeKind;
using sonora::PlaybackPlan;
using sonora::PlanNode;
using sonora::Session;

constexpr uint32_t kBlock = 64;
constexpr uint32_t kMaxCallback = 512;

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

    uint32_t block_frames() const noexcept SONORA_RT override { return 0; }
    // A node not run: silence (the real runner fades; plugin_client tests it).
    void skip(const PlanNode&, float* const out[2], uint32_t frames) noexcept SONORA_RT override {
        for (uint32_t i = 0; i < frames; ++i) out[0][i] = out[1][i] = 0.0f;
        ++skips;
    }
    long skips = 0;

    void run(const PlanNode& node, const float* const in[2], float* const out[2], uint32_t frames,
             const sonora::ChainParamEvent*, uint32_t) noexcept SONORA_RT override {
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

    PlaybackPlan* plan(const std::vector<ChainBinding>& chains, DelayLinePool& pool,
                       sonora::Compensation* comp = nullptr,
                       std::vector<uint32_t>* shortfall = nullptr) {
        return playback_test::chain_plan(*session, chains, pool, comp, shortfall);
    }

    bool publish(PlaybackPlan* p) { return playback_test::publish_plan(*session, p); }

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
        // The backend stamps every period (contracts/backend, callback_ns).
        b.callback_ns = 1'000'000'000 + static_cast<uint64_t>(frame) * 1000;
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
    c.node_state = slot;
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

// PR #59 re-review (2): a bound chain with no runner installed is not run.
// The plan delays the dry track by its 512 frames, so playing its input
// would put it on the bus 512 frames early: it is silence, counted, and the
// dry track keeps its compensation.
int test_bound_chain_without_a_runner_is_silence() {
    Rig rig(2, 16384);
    rig.session->tracks()[0]->set_pan(-1.0f);
    rig.session->tracks()[1]->set_pan(1.0f);
    DelayLinePool pool(rig.session->delay_lines());
    CHECK(rig.publish(rig.plan({chain(0, 512), ChainBinding{}}, pool)));
    reset_heap_counts();
    for (int blk = 0; blk < 20; ++blk) {
        const int64_t at = rig.frame;
        rig.block();
        for (uint32_t i = 0; i < kBlock; ++i) {
            CHECK(rig.out[0][i] == 0.0f);
            CHECK(rig.out[1][i] == signal(at + i - 512));
        }
    }
    CHECK(heap_untouched());
    CHECK(rig.session->unrun_chain_blocks() == 20);
    return 0;
}

// A chain bound to no slot: laid out with latency 0, the chain passes its
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
    CHECK(rig.publish(rig.plan({chain(0, 256), ChainBinding{}}, pool)));
    for (int blk = 0; blk < 8; ++blk) rig.block();
    CHECK(runner.calls == 8 && runner.begins == 8 && runner.ends == 8);

    reset_heap_counts();
    const int64_t at = rig.frame;
    rig.block(2 * kBlock);
    CHECK(heap_untouched());
    CHECK(runner.calls == 8 && runner.begins == 8 && runner.ends == 8);
    CHECK(rig.session->chain_bypassed_blocks() == 1);
    // The chain's track is silence, never its dry input 256 frames early
    // (PR #59 re-review); the dry track keeps its compensation.
    CHECK(rig.session->unrun_chain_blocks() == 2);  // one per slice
    for (uint32_t i = 0; i < 2 * kBlock; ++i) {
        CHECK(rig.out[0][i] == 0.0f);
        CHECK(rig.out[1][i] == signal(at + i - 256));
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

    // PR #77 RT audit L4: more nodes than the session has scratch buffers,
    // every one of them in bounds by sharing buffer 0. The builder never
    // makes one; adopting it indexed the per-node parameter order past its
    // end. Refused as a whole.
    sonora::PlanSizes sizes;
    sizes.tracks = 1;
    // The session sizes its buffers for every track with a chain and every
    // aux bus (ADR-0022 §5).
    sizes.nodes = sonora::graph_nodes(1, 1) + 2 * rig.session->max_buses() + 2;
    PlaybackPlan* shared = sonora::create_plan(sizes);
    shared->epoch = rig.session->plans().next_epoch();
    shared->chunk_frames = 256;
    shared->tracks[0].engine_track = 0;
    shared->tracks[0].channels = 1;
    for (uint32_t ni = 0; ni < shared->node_count; ++ni) {
        shared->nodes[ni].kind = sonora::NodeKind::BusSum;
        shared->nodes[ni].scratch_buffer = 0;
        shared->nodes[ni].first_input_edge = 0;
        shared->nodes[ni].input_edge_count = 0;
    }
    shared->last_insert_node = sonora::kNoNode;
    CHECK(rig.publish(shared));
    reset_heap_counts();
    rig.block();
    CHECK(heap_untouched());
    CHECK(rig.session->invalid_plan_blocks() == 2);
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(rig.out[0][i] == 0.0f && rig.out[1][i] == 0.0f);
    return 0;
}

// Review of PR #59 (rt-engineer ruling 1): a binding that names a slot and
// a latency but has no node state cannot be handed to a host, so the track
// plays dry. Laid out as bound, compensate() delayed every other track by
// its latency and the dry one reached the bus that much early. The builder
// lays it out unbound: no slot, latency 0. So is a binding with no slot,
// whatever latency it carries. Driven through the real PluginChainRunner,
// which before the ruling played such a node dry with its latency planned.
int test_no_node_state_is_unbound_and_never_early() {
    Rig rig(3, 16384);
    sonora::plugin_client::ClientConfig ccfg;
    ccfg.block_frames = kBlock;
    ccfg.max_channels = 4;
    ccfg.max_stores = 2;
    ccfg.store_slots = 16;
    ccfg.max_nodes = 4;
    sonora::plugin_client::PluginClient client(ccfg);
    sonora::PluginChainRunner runner(client);
    rig.session->set_insert_chains(&runner);
    // 0 and 2 left, 1 right: aligned and dry, left is exactly twice right.
    rig.session->tracks()[0]->set_pan(-1.0f);
    rig.session->tracks()[1]->set_pan(1.0f);
    rig.session->tracks()[2]->set_pan(-1.0f);
    ChainBinding stateless = chain(0, 2048);
    stateless.node_state = sonora::kNoNodeState;
    ChainBinding slotless = chain(1, 512);
    slotless.channel_slot = sonora::kNoChannelSlot;
    DelayLinePool pool(rig.session->delay_lines());
    sonora::Compensation comp;
    PlaybackPlan* p = rig.plan({stateless, ChainBinding{}, slotless}, pool, &comp);
    CHECK(p->nodes[1].kind == NodeKind::InsertChain);
    CHECK(p->nodes[1].channel_slot == sonora::kNoChannelSlot);
    CHECK(p->nodes[1].latency_frames == 0);
    CHECK(p->nodes[1].node_state == sonora::kNoNodeState);
    CHECK(p->nodes[5].kind == NodeKind::InsertChain);
    CHECK(p->nodes[5].channel_slot == sonora::kNoChannelSlot);
    CHECK(p->nodes[5].latency_frames == 0);
    CHECK(comp.compensation_frames == 0);
    CHECK(p->compensation_frames == 0);
    CHECK(rig.publish(p));

    reset_heap_counts();
    for (int blk = 0; blk < 60; ++blk) {
        const int64_t at = rig.frame;
        rig.block();
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float want = signal(at + i);
            CHECK(rig.out[1][i] == want);
            CHECK(rig.out[0][i] == want + want);
        }
    }
    CHECK(heap_untouched());
    // The session's own pass-through played them; the client never saw them.
    CHECK(runner.collected() + runner.missed() + runner.removed() == 0);
    CHECK(runner.no_node_state() == 0 && runner.unsupported() == 0);
    CHECK(client.callbacks() == 60);
    return 0;
}

// RT audit of PR #59 (finding 2): a zero-frame callback ran begin() and
// never end(), since the mix loop has nothing to mix. The client's
// in_callback stayed raised and the next block counted a miss. The runner
// sees neither for it.
int test_zero_frame_callback_neither_begins_nor_ends() {
    Rig rig(2, 16384);
    DelayRunner runner(1, 4096);
    rig.session->set_insert_chains(&runner);
    DelayLinePool pool(rig.session->delay_lines());
    CHECK(rig.publish(rig.plan({chain(0, 128), ChainBinding{}}, pool)));
    rig.block();
    CHECK(runner.begins == 1 && runner.ends == 1);
    reset_heap_counts();
    rig.block(0);
    CHECK(heap_untouched());
    CHECK(runner.begins == 1 && runner.ends == 1 && runner.calls == 1);
    CHECK(rig.session->chain_bypassed_blocks() == 0);
    rig.block();
    CHECK(runner.begins == 2 && runner.ends == 2 && runner.calls == 2);
    CHECK(runner.sequence_errors == 0);
    return 0;
}

}  // namespace

int main() {
    std::printf("plan_pdc: heap counter covers %s\n", playback_test::heap_counter_scope());
    if (test_2048_lands_aligned()) return 1;
    if (test_over_the_maximum_is_clamped_and_reported()) return 1;
    if (test_pool_cap()) return 1;
    if (test_delay_line_pool()) return 1;
    if (test_swap_with_latency_change()) return 1;
    if (test_unbound_chain_passes_through()) return 1;
    if (test_line_restarts_after_a_gap()) return 1;
    if (test_stable_ids_follow_the_track()) return 1;
    if (test_long_callback_bypasses_chains()) return 1;
    if (test_bound_chain_without_a_runner_is_silence()) return 1;
    if (test_out_of_bounds_plan_is_silence()) return 1;
    if (test_zero_frame_callback_neither_begins_nor_ends()) return 1;
    if (test_no_node_state_is_unbound_and_never_early()) return 1;
    std::printf("plan_pdc: ok\n");
    return 0;
}
