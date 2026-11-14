// SPDX-License-Identifier: GPL-3.0-or-later
//
// A strip rerouted at a plan swap crossfades instead of cutting (ADR-0202):
// on an undelayed edge, and through PDC delay lines, where the old route's
// in-flight frames play out and the new route starts from a fresh line. A
// steady input makes any cut a step in the output; no step may be larger
// than one increment of the fade.
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#define PLAYBACK_TEST_COUNT_HEAP 1
#include "playback_test.h"
#include "sonora/insert_chain_runner.h"
#include "sonora/plan_graph.h"
#include "sonora/session.h"

namespace {

using playback_test::heap_untouched;
using playback_test::reset_heap_counts;
using playback_test::t_counting;
using sonora::ChainBinding;
using sonora::DelayLinePool;
using sonora::MixLayout;
using sonora::PlaybackPlan;
using sonora::PlanNode;
using sonora::Session;

constexpr uint32_t kBlock = 64;
constexpr uint32_t kNoBus = sonora::kNoBus;

// Each frame comes out latency_frames late, as test_routing's runner.
class DelayRunner final : public sonora::InsertChainRunner {
public:
    explicit DelayRunner(uint32_t capacity) : cap_(capacity), ring_(capacity, 0.0f) {}
    void begin(uint64_t) noexcept SONORA_RT override {}
    void end() noexcept SONORA_RT override {}
    uint32_t block_frames() const noexcept SONORA_RT override { return 0; }
    void skip(const PlanNode&, float* const out[2], uint32_t frames) noexcept SONORA_RT override {
        for (uint32_t i = 0; i < frames; ++i) out[0][i] = out[1][i] = 0.0f;
    }
    void run(const PlanNode& node, const float* const in[2], float* const out[2], uint32_t frames,
             const sonora::ChainParamEvent*, uint32_t) noexcept SONORA_RT override {
        for (uint32_t i = 0; i < frames; ++i) {
            ring_[pos_] = in[0][i];
            out[0][i] = ring_[(pos_ + cap_ - node.latency_frames) % cap_];
            out[1][i] = out[0][i];
            pos_ = (pos_ + 1) % cap_;
        }
    }

private:
    uint32_t cap_;
    std::vector<float> ring_;
    uint32_t pos_ = 0;
};

// A steady 1.0 on every track's input, monitored, so the output is the
// routes' gains alone.
struct Rig {
    std::unique_ptr<Session> session;
    std::vector<std::vector<float>> in;
    std::vector<const float*> in_ptrs;
    std::vector<float> out[2];
    float* out_ptrs[2];
    std::vector<float> heard;   // the left output, every frame played

    explicit Rig(int tracks) {
        sonora::SessionConfig cfg;
        cfg.dir = "test_edge_ramp_unused";
        cfg.track_count = tracks;
        cfg.ring_seconds = 0.1;
        cfg.playback.chunk_frames = 256;
        cfg.playback.window_seconds = 0.02;
        cfg.playback.pool_chunks = 64;
        cfg.playback.max_block_frames = 256;
        cfg.playback.max_delay_frames = 4096;
        cfg.playback.max_buses = 2;
        cfg.playback.max_delayed_edges = 64;
        session = std::make_unique<Session>(cfg);
        in.assign(tracks, std::vector<float>(kBlock, 1.0f));
        for (auto& v : in) in_ptrs.push_back(v.data());
        for (int o = 0; o < 2; ++o) {
            out[o].assign(kBlock, 0.0f);
            out_ptrs[o] = out[o].data();
        }
        for (auto& t : session->tracks()) {
            t->set_monitor(sonora::Track::Monitor::Input);
            t->set_pan(-1.0f);
        }
    }

    PlaybackPlan* plan(const std::vector<ChainBinding>& chains, DelayLinePool& pool,
                       std::vector<uint32_t> track_out) {
        const auto tracks = static_cast<uint32_t>(session->tracks().size());
        std::vector<ChainBinding> c = chains;
        c.resize(tracks);
        track_out.resize(tracks, kNoBus);
        const std::vector<uint32_t> order{0};
        const std::vector<uint32_t> bus_out{kNoBus};
        const std::vector<uint16_t> bus_hw{sonora::kNoHardware};
        MixLayout mix;
        mix.track_outputs = track_out.data();
        mix.bus_outputs = bus_out.data();
        mix.bus_hardware = bus_hw.data();
        mix.bus_count = 1;
        mix.bus_order = order.data();
        const sonora::GraphSize g = sonora::mix_graph_size(tracks, c.data(), mix);
        sonora::PlanSizes sizes;
        sizes.tracks = tracks;
        sizes.nodes = g.nodes;
        sizes.edges = g.edges;
        PlaybackPlan* p = sonora::create_plan(sizes);
        p->epoch = session->plans().next_epoch();
        p->chunk_frames = 256;
        p->pan_exponent = 0.0f;
        for (uint32_t t = 0; t < tracks; ++t) {
            p->tracks[t].engine_track = t;
            p->tracks[t].channels = 1;
        }
        sonora::layout_mix_graph(*p, c.data(), mix);
        sonora::compensate(*p, session->max_delay_frames(), pool);
        return p;
    }

    bool publish(PlaybackPlan* p) { return playback_test::publish_plan(*session, p); }

    void block() {
        sonora::backend::AudioBlock b{};
        b.inputs = in_ptrs.data();
        b.input_count = static_cast<uint32_t>(in_ptrs.size());
        b.outputs = out_ptrs;
        b.output_count = 2;
        b.nframes = kBlock;
        b.callback_ns = 1'000'000'000 + heard.size() * 1000;
        t_counting = true;
        session->process(b);
        t_counting = false;
        heard.insert(heard.end(), out[0].begin(), out[0].end());
    }
};

ChainBinding latent_chain(uint32_t latency) {
    ChainBinding c;
    c.active = true;
    c.chain_id = 100;
    c.channel_slot = 0;
    c.param_store = 7;
    c.node_state = 0;
    c.latency_frames = latency;
    return c;
}

// The largest step between neighbouring frames of `v` from `from` on.
float largest_step(const std::vector<float>& v, std::size_t from, std::size_t& at) {
    float worst = 0.0f;
    for (std::size_t i = from + 1; i < v.size(); ++i) {
        const float d = std::fabs(v[i] - v[i - 1]);
        if (d > worst) {
            worst = d;
            at = i;
        }
    }
    return worst;
}

// Plays until the fade and any line behind it are long over.
void play_out(Rig& rig, uint32_t extra_frames) {
    const uint32_t frames = rig.session->edge_ramp_frames() + extra_frames + 4 * kBlock;
    for (uint32_t f = 0; f < frames; f += kBlock) rig.block();
}

// Track on the Master, then through a bus at half gain: a cut steps by half
// the input at the swap; the fade moves by at most one increment a frame and
// settles on the bus's level.
int test_undelayed_reroute_ramps() {
    Rig rig(1);
    Session& s = *rig.session;
    s.set_aux(0, 0.5f, 0.0f, false);
    DelayLinePool pool(s.delay_lines());
    CHECK(rig.publish(rig.plan({}, pool, {kNoBus})));
    for (int i = 0; i < 4; ++i) rig.block();
    CHECK(rig.heard.back() == 1.0f);
    const std::size_t swap = rig.heard.size() - 1;
    CHECK(rig.publish(rig.plan({}, pool, {0})));
    reset_heap_counts();
    play_out(rig, 0);
    CHECK(heap_untouched());
    std::size_t at = 0;
    const float step = largest_step(rig.heard, swap, at);
    const float increment = 1.0f / static_cast<float>(s.edge_ramp_frames());
    if (step > increment * 1.001f) {
        std::fprintf(stderr, "  step %g at frame %zu (swap at %zu), one increment %g\n", step, at,
                     swap + 1, increment);
        return 1;
    }
    // A fade, not a cut that happens to be small: half way, both routes.
    const float mid = rig.heard[swap + 1 + s.edge_ramp_frames() / 2];
    CHECK(mid > 0.7f && mid < 0.8f);
    CHECK(rig.heard.back() == 0.5f);
    return 0;
}

// A dry track rerouted from the Master to a bus while another track plays
// behind a 100-frame insert: the dry track's old edge is 100 frames late
// through a line, and its new path takes a fresh line on the bus's return.
// The old line plays out its frames as the old route fades, the new line
// fades in from silence, and the two cross on the same frames.
int test_reroute_through_delay_lines_ramps() {
    Rig rig(2);
    Session& s = *rig.session;
    DelayRunner runner(4096);
    s.set_insert_chains(&runner);
    s.set_aux(0, 0.5f, 0.0f, false);
    DelayLinePool pool(s.delay_lines());
    const std::vector<ChainBinding> chains{ChainBinding{}, latent_chain(100)};
    PlaybackPlan* p = rig.plan(chains, pool, {kNoBus, kNoBus});
    CHECK(p->compensation_frames == 100);
    CHECK(rig.publish(p));
    for (int i = 0; i < 6; ++i) rig.block();
    CHECK(rig.heard.back() == 2.0f);
    const std::size_t swap = rig.heard.size() - 1;
    PlaybackPlan* q = rig.plan(chains, pool, {0, kNoBus});
    CHECK(q->compensation_frames == 100 && q->compensation_shortfall_frames == 0);
    CHECK(rig.publish(q));
    reset_heap_counts();
    play_out(rig, 100);
    CHECK(heap_untouched());
    std::size_t at = 0;
    const float step = largest_step(rig.heard, swap, at);
    const float increment = 1.0f / static_cast<float>(s.edge_ramp_frames());
    if (step > increment * 1.001f) {
        std::fprintf(stderr, "  step %g at frame %zu (swap at %zu), one increment %g\n", step, at,
                     swap + 1, increment);
        return 1;
    }
    // The frames in flight play whole: the fade reaches the output 100
    // frames after the swap, with the rest of the aligned mix.
    for (std::size_t i = swap + 1; i <= swap + 100; ++i) CHECK(rig.heard[i] == 2.0f);
    CHECK(rig.heard[swap + 101] < 2.0f);
    CHECK(rig.heard.back() == 1.5f);
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    int run = 0;
    const auto t = [&](int (*f)()) {
        ++run;
        failed += f();
    };
    t(test_undelayed_reroute_ramps);
    t(test_reroute_through_delay_lines_ramps);
    if (failed != 0) {
        std::fprintf(stderr, "edge_ramp: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("edge_ramp: %d passed (heap counter: %s)\n", run, playback_test::heap_counter_scope());
    return 0;
}
