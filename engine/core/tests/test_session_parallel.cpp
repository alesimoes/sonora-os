// SPDX-License-Identifier: GPL-3.0-or-later
//
// Session::process() through the processing graph (ADR-0063): with real
// graph workers it plays the serial output bit for bit and touches no heap;
// a dropped node is ramped silence in a buffer of its own, counted, and
// never costs a recorded sample; a pool that cannot start fails the
// session's creation. Drops come from the fake scheduler, never from time:
// the worker runs get a deadline an hour away, so only a hung worker could
// make one late.
#define PLAYBACK_TEST_COUNT_HEAP 1
#include "playback_test.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "graph_schedule_fake.h"
#include "sonora/graph_levels.h"

namespace sonora {
struct SessionTestAccess {
    static bool levels_ok(const Session& s) { return s.graph_levels_ok_; }
    static const float* scratch(Session& s, uint32_t buffer, uint32_t side) {
        return s.scratch(buffer, side);
    }
};
}  // namespace sonora

namespace {

using namespace playback_test;
using sonora::NodeKind;
using sonora::Session;
using sonora::SessionTestAccess;

constexpr uint32_t kTracks = 6;
constexpr uint32_t kFrames = 128;
constexpr uint32_t kBlocks = 24;

sonora::SessionConfig config(uint32_t workers, sonora::GraphScheduler* scheduler = nullptr) {
    sonora::SessionConfig cfg;
    // process() never touches the folder; nothing is created there.
    cfg.dir = "test_session_parallel_unused";
    cfg.track_count = static_cast<int>(kTracks);
    cfg.ring_seconds = 1.0;
    cfg.playback.max_block_frames = kFrames;
    cfg.playback.graph_workers = workers;
    cfg.graph_scheduler = scheduler;
    return cfg;
}

// Every track armed and monitoring its input through an unbound chain:
// TrackSource -> InsertChain -> TrackFader, the first and last of which a
// worker may run, laid out and levelled as engine/control builds it.
bool prepare(Session& s, sonora::DelayLinePool& pool) {
    for (uint32_t t = 0; t < kTracks; ++t) {
        s.tracks()[t]->set_rec_armed(true);
        s.tracks()[t]->set_monitor(sonora::Track::Monitor::Input);
        s.tracks()[t]->set_input(t);
    }
    std::vector<sonora::ChainBinding> chains(kTracks);
    for (sonora::ChainBinding& c : chains) c.active = true;
    sonora::PlaybackPlan* p = chain_plan(s, chains, pool);
    if (!sonora::assign_plan_levels(*p) || p->level_count < 3) {
        sonora::destroy_plan(p);
        return false;
    }
    return publish_plan(s, p);
}

// Engine track `track`'s node of `kind` in the playing plan.
uint32_t node_of(Session& s, NodeKind kind, uint32_t track) {
    const sonora::PlaybackPlan& p = *s.plans().current();
    for (uint32_t i = 0; i < p.node_count; ++i) {
        if (p.nodes[i].kind == kind && p.tracks[p.nodes[i].track].engine_track == track) return i;
    }
    return ~uint32_t{0};
}

// One multiply, so no call site can fuse it differently (an FMA would make
// two evaluations differ in the last bit), and distinct per track and frame.
float input_value(uint32_t track, uint32_t block, uint32_t frame) {
    return static_cast<float>((track + 1) * 100000 + block * kFrames + frame) * 1e-6f;
}

struct Run {
    std::vector<float> out;                    // L then R, block after block
    std::vector<std::vector<float>> captured;  // per track
    bool heap_untouched = true;
};

// kBlocks blocks while recording. `silent_track` feeds zeros instead of its
// input; `stall_from`..`stall_to` stall that track's TrackSource on `fake`.
int roll(Session& s, Run& run, uint32_t silent_track = kTracks,
         graph_test::FakeGraphScheduler* fake = nullptr, uint32_t stall_from = 0,
         uint32_t stall_to = 0, uint32_t stall_track = 0) {
    s.transport().set_state(sonora::TransportState::Recording);
    std::vector<std::vector<float>> in(kTracks, std::vector<float>(kFrames));
    std::vector<const float*> inputs(kTracks);
    std::vector<float> left(kFrames);
    std::vector<float> right(kFrames);
    float* outputs[2] = {left.data(), right.data()};
    const uint32_t stalled = fake != nullptr ? node_of(s, NodeKind::TrackSource, stall_track) : 0;
    for (uint32_t b = 0; b < kBlocks; ++b) {
        for (uint32_t t = 0; t < kTracks; ++t) {
            for (uint32_t f = 0; f < kFrames; ++f) {
                in[t][f] = t == silent_track ? 0.0f : input_value(t, b, f);
            }
            inputs[t] = in[t].data();
        }
        if (fake != nullptr) fake->set_stalled(stalled, b >= stall_from && b < stall_to);
        sonora::backend::AudioBlock block{};
        block.inputs = inputs.data();
        block.input_count = kTracks;
        block.outputs = outputs;
        block.output_count = 2;
        block.nframes = kFrames;
        block.callback_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                (std::chrono::steady_clock::now() + std::chrono::hours(1)).time_since_epoch())
                .count());
        sonora::backend::ProcessCallback& cb = s;
        reset_heap_counts();
        t_counting = true;
        cb.process(block);
        t_counting = false;
        run.heap_untouched = run.heap_untouched && playback_test::heap_untouched();
        run.out.insert(run.out.end(), left.begin(), left.end());
        run.out.insert(run.out.end(), right.begin(), right.end());
    }
    run.captured.assign(kTracks, {});
    for (uint32_t t = 0; t < kTracks; ++t) {
        sonora::Track& track = *s.tracks()[t];
        run.captured[t].resize(track.capture_ring().read_space());
        track.capture_ring().read(run.captured[t].data(), run.captured[t].size());
    }
    return 0;
}

bool same_bits(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

// Two real workers beside the audio thread: the serial output bit for bit,
// nothing dropped, and no allocation or free on the audio thread.
int test_workers_equal_serial_without_heap() {
    Session serial(config(0));
    Session parallel(config(2));
    sonora::DelayLinePool pool_a(serial.delay_lines());
    sonora::DelayLinePool pool_b(parallel.delay_lines());
    CHECK(prepare(serial, pool_a));
    CHECK(prepare(parallel, pool_b));
    CHECK(serial.graph_workers() == 0);
    CHECK(parallel.graph_workers() == 2);
    Run a;
    Run b;
    CHECK(roll(serial, a) == 0);
    CHECK(roll(parallel, b) == 0);
    // Levels adopted, so the scheduler, not the serial walk, ran the nodes.
    CHECK(SessionTestAccess::levels_ok(parallel));
    CHECK(!SessionTestAccess::levels_ok(serial));
    CHECK(same_bits(a.out, b.out));
    for (uint32_t t = 0; t < kTracks; ++t) {
        CHECK(a.captured[t].size() == kBlocks * kFrames);
        CHECK(same_bits(a.captured[t], b.captured[t]));
    }
    CHECK(parallel.graph_drops().dropped == 0 && parallel.graph_drops().skipped == 0);
    CHECK(parallel.graph_late_results() == 0);
    CHECK(b.heap_untouched);
    // Signal reached the output, so "equal" is not two silences.
    bool any = false;
    for (float v : b.out) any = any || v != 0.0f;
    CHECK(any);
    return 0;
}

// Track 0's source stalled for the whole roll: dropped 8 times, then down
// and skipped. The take is every sample the inputs carried, as without a
// drop, and the mix is the mix of a roll whose track 0 was silent.
int test_drops_never_touch_the_take() {
    Session plain(config(0));
    Session silent(config(0));
    graph_test::FakeGraphScheduler fake(4096, 2);
    Session dropping(config(0, &fake));
    sonora::DelayLinePool pool_a(plain.delay_lines());
    sonora::DelayLinePool pool_b(silent.delay_lines());
    sonora::DelayLinePool pool_c(dropping.delay_lines());
    CHECK(prepare(plain, pool_a));
    CHECK(prepare(silent, pool_b));
    CHECK(prepare(dropping, pool_c));
    Run a;
    Run s;
    Run d;
    CHECK(roll(plain, a) == 0);
    CHECK(roll(silent, s, 0) == 0);
    CHECK(roll(dropping, d, kTracks, &fake, 0, kBlocks, 0) == 0);
    CHECK(SessionTestAccess::levels_ok(dropping));
    for (uint32_t t = 0; t < kTracks; ++t) {
        CHECK(d.captured[t].size() == kBlocks * kFrames);
        CHECK(same_bits(a.captured[t], d.captured[t]));
    }
    CHECK(same_bits(s.out, d.out));
    CHECK(!same_bits(a.out, d.out));
    CHECK(dropping.graph_drops().dropped == sonora::kGraphDownAfterDrops);
    CHECK(dropping.graph_drops().skipped == kBlocks - sonora::kGraphDownAfterDrops);
    CHECK(d.heap_untouched);
    return 0;
}

// A drop mid-roll ramps from the last frame the node played to silence over
// the block, in the drop buffer: the node's own scratch, which a late
// worker may still be writing, keeps what run() last left in it. The node
// plays again the block its stall clears.
int test_drop_ramps_in_its_own_buffer() {
    graph_test::FakeGraphScheduler fake(4096, 2);
    Session s(config(0, &fake));
    sonora::DelayLinePool pool(s.delay_lines());
    CHECK(prepare(s, pool));
    Run r;
    // Track 1 stalls in this block only.
    constexpr uint32_t kStall = 6;
    CHECK(roll(s, r, kTracks, &fake, kStall, kStall + 1, 1) == 0);
    CHECK(s.graph_drops().dropped == 1 && s.graph_drops().skipped == 0);
    const sonora::PlaybackPlan& p = *s.plans().current();
    const uint32_t source = node_of(s, NodeKind::TrackSource, 1);
    CHECK(source < p.node_count);
    // The last block ran: its scratch holds the input of the last block.
    const float* scratch = SessionTestAccess::scratch(s, p.nodes[source].scratch_buffer, 0);
    for (uint32_t f = 0; f < kFrames; ++f) {
        CHECK(scratch[f] == input_value(1, kBlocks - 1, f));
    }
    // The mix of the stalled block against the same roll with no stall:
    // track 1's share fades linearly from its last frame.
    graph_test::FakeGraphScheduler calm(4096, 2);
    Session ref(config(0, &calm));
    sonora::DelayLinePool ref_pool(ref.delay_lines());
    CHECK(prepare(ref, ref_pool));
    Run q;
    CHECK(roll(ref, q) == 0);
    Session without(config(0));
    sonora::DelayLinePool without_pool(without.delay_lines());
    CHECK(prepare(without, without_pool));
    Run w;
    CHECK(roll(without, w, 1) == 0);
    // Left side of block kStall: with = rest + share; share of track 1 in
    // the reference is q - w, its last frame of block kStall - 1 is the
    // ramp's start.
    const std::size_t at = static_cast<std::size_t>(kStall) * 2 * kFrames;
    const std::size_t before = static_cast<std::size_t>(kStall - 1) * 2 * kFrames;
    const float last = q.out[before + kFrames - 1] - w.out[before + kFrames - 1];
    CHECK(last != 0.0f);
    for (uint32_t f = 0; f < kFrames; ++f) {
        const float share = r.out[at + f] - w.out[at + f];
        const float want = last * (1.0f - static_cast<float>(f + 1) / static_cast<float>(kFrames));
        CHECK(std::fabs(share - want) <= 1e-4f * std::fabs(last) + 1e-6f);
    }
    // The block after: playing again, as in the reference.
    const std::size_t next = static_cast<std::size_t>(kStall + 1) * 2 * kFrames;
    CHECK(std::memcmp(r.out.data() + next, q.out.data() + next, 2 * kFrames * sizeof(float)) == 0);
    return 0;
}

// ADR-0063 §6: a pool that cannot start fails the session's creation with
// the reason; it never falls back to serial. A SCHED_FIFO priority past
// the range fails on every machine, privileged or not.
int test_pool_start_failure_fails_creation() {
    sonora::SessionConfig cfg = config(2);
    cfg.playback.graph_worker_priority = 1000;
    std::string why;
    try {
        Session s(cfg);
    } catch (const std::runtime_error& e) {
        why = e.what();
    }
    CHECK(why.find("graph worker") != std::string::npos);
    return 0;
}

}  // namespace

int main() {
    if (test_workers_equal_serial_without_heap() != 0) return 1;
    if (test_drops_never_touch_the_take() != 0) return 1;
    if (test_drop_ramps_in_its_own_buffer() != 0) return 1;
    if (test_pool_start_failure_fails_creation() != 0) return 1;
    std::printf("session_parallel: ok (heap counter: %s)\n", heap_counter_scope());
    return 0;
}
