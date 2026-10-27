// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sends and aux buses through Session::process() (ADR-0022): the summing
// math, pre against post fader, the send's pan following the track's or its
// own, mutes, a bus feeding a bus, a fixed summing order, the same output
// whatever the block split, a new send ramped in from silence, PDC over the
// wet paths, a plan with a send index out of range refused, and no heap
// touched on the audio thread through any of it. Then #163's review: a send
// reassigned to another bus ramps in, a first send keeps the dry path's
// delay line, a kept send survives a swap, and the PDC line budget.
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
using sonora::NodeKind;
using sonora::PlaybackPlan;
using sonora::PlanNode;
using sonora::SendBinding;
using sonora::Session;

constexpr uint32_t kBlock = 64;
constexpr uint32_t kMaxCallback = 512;

// Exact in float, distinct per frame; times a power of two still exact.
float signal(int64_t t) { return 1.0f + static_cast<float>(t % 4096) / 4096.0f; }

// A host with a declared latency, as test_plan_pdc's: each frame comes out
// latency_frames late.
class DelayRunner final : public sonora::InsertChainRunner {
public:
    DelayRunner(uint32_t slots, uint32_t capacity)
        : cap_(capacity), ring_(static_cast<std::size_t>(slots) * capacity, 0.0f), pos_(slots, 0) {}
    void begin(uint64_t) noexcept SONORA_RT override {}
    void end() noexcept SONORA_RT override {}
    uint32_t block_frames() const noexcept SONORA_RT override { return 0; }
    void skip(const PlanNode&, float* const out[2], uint32_t frames) noexcept SONORA_RT override {
        for (uint32_t i = 0; i < frames; ++i) out[0][i] = out[1][i] = 0.0f;
    }
    void run(const PlanNode& node, const float* const in[2], float* const out[2], uint32_t frames,
             const sonora::ChainParamEvent*, uint32_t) noexcept SONORA_RT override {
        float* ring = ring_.data() + static_cast<std::size_t>(node.channel_slot) * cap_;
        uint32_t pos = pos_[node.channel_slot];
        for (uint32_t i = 0; i < frames; ++i) {
            ring[pos] = in[0][i];
            out[0][i] = ring[(pos + cap_ - node.latency_frames) % cap_];
            out[1][i] = out[0][i];
            pos = (pos + 1) % cap_;
        }
        pos_[node.channel_slot] = pos;
    }

private:
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

    explicit Rig(int tracks, uint32_t max_block = 256, uint32_t max_buses = 4,
                 uint32_t max_delayed_edges = 128) {
        sonora::SessionConfig cfg;
        cfg.dir = "test_sends_unused";
        cfg.track_count = tracks;
        cfg.ring_seconds = 0.1;
        cfg.playback.chunk_frames = 256;
        cfg.playback.window_seconds = 0.02;
        cfg.playback.pool_chunks = 64;
        cfg.playback.max_block_frames = max_block;
        cfg.playback.max_delay_frames = 4096;
        cfg.playback.max_buses = max_buses;
        cfg.playback.max_delayed_edges = max_delayed_edges;
        session = std::make_unique<Session>(cfg);
        in.assign(tracks, std::vector<float>(kMaxCallback, 0.0f));
        for (auto& v : in) in_ptrs.push_back(v.data());
        for (int o = 0; o < 2; ++o) {
            out[o].assign(kMaxCallback, 0.0f);
            out_ptrs[o] = out[o].data();
        }
        // Every track hears its input: the graph is under test.
        for (auto& t : session->tracks()) t->set_monitor(sonora::Track::Monitor::Input);
    }

    // A plan with no regions, the given chains, sends and buses, laid out
    // and compensated as engine/control's builder does.
    // `pan_exponent` 0 makes a mono pan at centre 1 on each side, for sums
    // that are exact.
    PlaybackPlan* plan(const std::vector<ChainBinding>& chains, const std::vector<SendBinding>& sends,
                       uint32_t buses, const std::vector<uint32_t>& order, DelayLinePool& pool,
                       float pan_exponent = 0.5f) {
        const auto tracks = static_cast<uint32_t>(session->tracks().size());
        std::vector<ChainBinding> c = chains;
        c.resize(tracks);
        MixLayout mix;
        mix.bus_count = buses;
        mix.bus_order = order.data();
        mix.sends = sends.data();
        mix.send_count = static_cast<uint32_t>(sends.size());
        const sonora::GraphSize g = sonora::mix_graph_size(tracks, c.data(), mix);
        sonora::PlanSizes sizes;
        sizes.tracks = tracks;
        sizes.nodes = g.nodes;
        sizes.edges = g.edges;
        PlaybackPlan* p = sonora::create_plan(sizes);
        p->epoch = session->plans().next_epoch();
        p->chunk_frames = 256;
        p->pan_exponent = pan_exponent;
        for (uint32_t t = 0; t < tracks; ++t) {
            p->tracks[t].engine_track = t;
            p->tracks[t].channels = 1;
        }
        sonora::layout_mix_graph(*p, c.data(), mix);
        sonora::compensate(*p, session->max_delay_frames(), pool);
        return p;
    }

    bool publish(PlaybackPlan* p) { return playback_test::publish_plan(*session, p); }

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
        b.callback_ns = 1'000'000'000 + static_cast<uint64_t>(frame) * 1000;
        t_counting = true;
        session->process(b);
        t_counting = false;
        frame += n;
    }
};

// Dry hard left, wet hard right: a mono track's pan law puts exactly 0 and
// 1 on the sides at the ends, so each side is one path.
int test_post_and_pre_fader() {
    Rig rig(1);
    Session& s = *rig.session;
    s.tracks()[0]->set_gain(0.5f);
    s.tracks()[0]->set_pan(-1.0f);
    s.set_send(0, 0, 0.25f, 1.0f, false, false, false);
    s.set_aux(0, 2.0f, 0.0f, false);
    DelayLinePool pool(s.delay_lines());
    const std::vector<uint32_t> order{0};
    CHECK(rig.publish(rig.plan({}, {{0, 0, 0}}, 1, order, pool)));

    reset_heap_counts();
    const auto settle = [&] {
        rig.block();
        rig.block();
    };
    settle();
    const auto check = [&](float dry, float wet) {
        const int64_t at = rig.frame;
        rig.block();
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float x = signal(at + i);
            if (rig.out[0][i] != x * dry || rig.out[1][i] != x * wet) {
                std::fprintf(stderr, "  frame %u: L %g want %g, R %g want %g\n", i,
                             rig.out[0][i], x * dry, rig.out[1][i], x * wet);
                return false;
            }
        }
        return true;
    };
    // The aux return is balance-panned at centre: a stereo bus at 0 is 1, 1.
    // Post-fader: input x track gain x send level x return gain.
    CHECK(check(0.5f, 0.5f * 0.25f * 2.0f));

    // Pre-fader: the track's gain is not in it.
    s.set_send(0, 0, 0.25f, 1.0f, false, false, true);
    settle();
    CHECK(check(0.5f, 0.25f * 2.0f));
    s.tracks()[0]->set_gain(0.125f);
    settle();
    CHECK(check(0.125f, 0.25f * 2.0f));

    // A muted track's pre-fader send is silent (ADR-0022 §6).
    s.tracks()[0]->set_muted(true);
    settle();
    CHECK(check(0.0f, 0.0f));
    s.tracks()[0]->set_muted(false);

    // The send's own mute, and the return's.
    s.set_send(0, 0, 0.25f, 1.0f, false, true, true);
    settle();
    CHECK(check(0.125f, 0.0f));
    s.set_send(0, 0, 0.25f, 1.0f, false, false, true);
    s.set_aux(0, 2.0f, 0.0f, true);
    settle();
    CHECK(check(0.125f, 0.0f));
    s.set_aux(0, 2.0f, 0.0f, false);

    // Following the main pan: hard left like the track, so the wet joins
    // the dry on the left.
    s.set_send(0, 0, 0.25f, 1.0f, true, false, true);
    settle();
    CHECK(check(0.125f + 0.25f * 2.0f, 0.0f));
    CHECK(heap_untouched());
    return 0;
}

// Two tracks into one bus, and that bus into a second, laid out after it;
// the sums are in the builder's order, tracks first, slots A to J.
int test_bus_into_bus_and_order() {
    Rig rig(2);
    Session& s = *rig.session;
    for (int t = 0; t < 2; ++t) {
        s.tracks()[t]->set_gain(0.0f);   // dry silent: only the wet is heard
    }
    s.set_send(0, 3, 0.5f, 0.0f, false, false, true);
    s.set_send(1, 1, 0.25f, 0.0f, false, false, true);
    // Bus 1 is listed first but fed by bus 0: it must be laid out after it.
    // Strip 2 is bus 0. Pre-fader, so bus 0's silent return is not in it.
    s.set_send(2 + 0, 0, 0.5f, 0.0f, false, false, true);
    s.set_aux(0, 0.0f, 0.0f, false);   // bus 0's return silent: heard through bus 1
    s.set_aux(1, 1.0f, 0.0f, false);
    DelayLinePool pool(s.delay_lines());
    const std::vector<uint32_t> order{0, 1};
    // Given out of order on purpose: the layout sorts.
    const std::vector<SendBinding> sends{{2, 0, 1}, {1, 1, 0}, {0, 3, 0}};
    PlaybackPlan* p = rig.plan({}, sends, 2, order, pool, 0.0f);
    // Two split tracks, two buses, the bus, out.
    CHECK(p->node_count == 2 * 2 + 2 * 2 + 2);
    const PlanNode& sum0 = p->nodes[4];
    CHECK(sum0.kind == NodeKind::AuxSum && sum0.track == 0 && sum0.input_edge_count == 2);
    CHECK(p->edges[sum0.first_input_edge].send == 0 * sonora::kStripSendSlots + 3);
    CHECK(p->edges[sum0.first_input_edge + 1].send == 1 * sonora::kStripSendSlots + 1);
    const PlanNode& sum1 = p->nodes[6];
    CHECK(sum1.kind == NodeKind::AuxSum && sum1.track == 1 && sum1.input_edge_count == 1);
    CHECK(p->edges[sum1.first_input_edge].from_node == 4);
    CHECK(p->nodes[8].kind == NodeKind::BusSum && p->nodes[8].input_edge_count == 4);
    CHECK(rig.publish(p));

    reset_heap_counts();
    rig.block();
    rig.block();
    const int64_t at = rig.frame;
    rig.block();
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float x = signal(at + i);
        const float want = (x * 0.5f + x * 0.25f) * 0.5f;
        CHECK(rig.out[0][i] == want && rig.out[1][i] == want);
    }
    CHECK(heap_untouched());

    // A bus order that is not topological is not laid out backwards: the
    // send from the later bus is left out, and the sizes agree.
    const std::vector<uint32_t> wrong{1, 0};
    MixLayout mix;
    mix.bus_count = 2;
    mix.bus_order = wrong.data();
    mix.sends = sends.data();
    mix.send_count = 3;
    std::vector<ChainBinding> none(2);
    const sonora::GraphSize g = sonora::mix_graph_size(2, none.data(), mix);
    CHECK(g.edges == 2 * 2 + 2 * 2 + 2 + 1);
    return 0;
}

// The output does not depend on how a callback is split into max_block
// pieces, ramps included; and in steady state not on the callback size.
int test_block_split_invariance() {
    std::vector<float> runs[2][2];
    const uint32_t pieces[2] = {256, 32};
    for (int r = 0; r < 2; ++r) {
        Rig rig(2, pieces[r]);
        Session& s = *rig.session;
        s.tracks()[0]->set_pan(-0.3f);
        s.tracks()[1]->set_pan(0.6f);
        s.set_send(0, 0, 0.7f, 0.0f, true, false, false);
        s.set_send(1, 2, 0.4f, -0.5f, false, false, true);
        s.set_aux(0, 0.9f, 0.2f, false);
        s.set_aux(1, 0.6f, -0.7f, false);
        DelayLinePool pool(s.delay_lines());
        const std::vector<uint32_t> order{0, 1};
        CHECK(rig.publish(rig.plan({}, {{0, 0, 0}, {1, 2, 0}}, 2, order, pool)));
        reset_heap_counts();
        for (int blk = 0; blk < 8; ++blk) {
            // Rides mid-run, so ramps are in what is compared.
            if (blk == 3) s.set_send(0, 0, 0.1f, 0.0f, true, false, false);
            // Assign Send: the same slot to the other bus, ramped in.
            if (blk == 4) {
                CHECK(rig.publish(rig.plan({}, {{0, 0, 1}, {1, 2, 0}}, 2, order, pool)));
            }
            if (blk == 5) s.set_aux(0, 0.3f, -0.4f, false);
            if (blk == 6) s.set_send(1, 2, 0.4f, -0.5f, false, true, true);
            rig.block(256);
            for (int o = 0; o < 2; ++o) {
                runs[r][o].insert(runs[r][o].end(), rig.out[o].begin(), rig.out[o].begin() + 256);
            }
        }
        CHECK(heap_untouched());
    }
    CHECK(runs[0][0] == runs[1][0]);
    CHECK(runs[0][1] == runs[1][1]);

    // Steady state: 4 callbacks of 64 make what one of 256 makes.
    std::vector<float> steady[2];
    for (int r = 0; r < 2; ++r) {
        Rig rig(1);
        Session& s = *rig.session;
        s.set_send(0, 0, 0.5f, 0.25f, false, false, false);
        DelayLinePool pool(s.delay_lines());
        const std::vector<uint32_t> order{0};
        CHECK(rig.publish(rig.plan({}, {{0, 0, 0}}, 1, order, pool)));
        rig.block(256);
        rig.block(256);
        for (int k = 0; k < (r == 0 ? 1 : 4); ++k) {
            const uint32_t n = r == 0 ? 256 : 64;
            rig.block(n);
            steady[r].insert(steady[r].end(), rig.out[1].begin(), rig.out[1].begin() + n);
        }
    }
    CHECK(steady[0] == steady[1]);
    return 0;
}

// A send that was not in the last callback's plan comes in from silence,
// ramped over the block: never a step in the signal.
int test_new_send_ramps_in() {
    Rig rig(1);
    Session& s = *rig.session;
    s.tracks()[0]->set_pan(-1.0f);
    s.set_send(0, 0, 1.0f, 1.0f, false, false, false);
    DelayLinePool pool(s.delay_lines());
    const std::vector<uint32_t> none;
    CHECK(rig.publish(rig.plan({}, {}, 0, none, pool)));
    rig.block();
    rig.block();
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(rig.out[1][i] == 0.0f);
    const std::vector<uint32_t> order{0};
    CHECK(rig.publish(rig.plan({}, {{0, 0, 0}}, 1, order, pool)));
    const int64_t at = rig.frame;
    rig.block();
    // The send and the return both ramp from 0: the first frame is tiny,
    // the last full, and every step is small.
    CHECK(rig.out[1][0] < 0.01f * signal(at));
    CHECK(rig.out[1][kBlock - 1] == signal(at + kBlock - 1));
    for (uint32_t i = 1; i < kBlock; ++i) {
        const float step = rig.out[1][i] - rig.out[1][i - 1];
        CHECK(step < 0.1f && step > -0.1f);
    }
    return 0;
}

// PDC over the wet path: a send from behind a 100-frame chain and one from
// a dry track meet aligned in the bus, and the return lands aligned with
// the dry paths.
int test_sends_are_compensated() {
    Rig rig(2);
    Session& s = *rig.session;
    DelayRunner runner(1, 4096);
    s.set_insert_chains(&runner);
    s.tracks()[0]->set_pan(-1.0f);
    s.tracks()[1]->set_pan(-1.0f);
    s.set_send(0, 0, 1.0f, 1.0f, false, false, true);
    s.set_send(1, 0, 1.0f, 1.0f, false, false, true);
    ChainBinding c;
    c.active = true;
    c.chain_id = 100;
    c.channel_slot = 0;
    c.param_store = 7;
    c.node_state = 0;
    c.latency_frames = 100;
    DelayLinePool pool(s.delay_lines());
    const std::vector<uint32_t> order{0};
    PlaybackPlan* p = rig.plan({c}, {{0, 0, 0}, {1, 0, 0}}, 1, order, pool);
    CHECK(p->compensation_frames == 100);
    CHECK(p->compensation_shortfall_frames == 0);
    CHECK(rig.publish(p));

    reset_heap_counts();
    long checked = 0;
    for (int blk = 0; blk < 12; ++blk) {
        const int64_t at = rig.frame;
        rig.block();
        if (blk < 4) continue;   // past the ramps and the delay lines' fill
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float want = signal(at + i - 100);
            CHECK(rig.out[0][i] == want + want);
            CHECK(rig.out[1][i] == want + want);
            ++checked;
        }
    }
    CHECK(checked > 400);
    CHECK(heap_untouched());
    return 0;
}

ChainBinding latent_chain(uint32_t slot, uint32_t latency) {
    ChainBinding c;
    c.active = true;
    c.chain_id = 100 + slot;
    c.channel_slot = slot;
    c.param_store = 7;
    c.node_state = 0;
    c.latency_frames = latency;
    return c;
}

// #163 review, finding 1: Assign Send is RemoveSend + AddSend in the same
// slot, so the send index is in both plans. The new bus must hear it ramp
// in from silence (ADR-0022 §6), not at the old bus's level.
int test_reassigned_send_ramps_in() {
    Rig rig(1);
    Session& s = *rig.session;
    s.tracks()[0]->set_gain(0.0f);   // dry silent; the send is pre-fader
    s.set_send(0, 0, 1.0f, 0.0f, false, false, true);
    s.set_aux(0, 1.0f, -1.0f, false);   // bus 0 heard on the left only
    s.set_aux(1, 1.0f, 1.0f, false);    // bus 1 on the right only
    DelayLinePool pool(s.delay_lines());
    const std::vector<uint32_t> order{0, 1};
    CHECK(rig.publish(rig.plan({}, {{0, 0, 0}}, 2, order, pool, 0.0f)));
    reset_heap_counts();
    for (int i = 0; i < 3; ++i) rig.block();
    CHECK(rig.out[0][kBlock - 1] == signal(rig.frame - 1));
    CHECK(rig.out[1][kBlock - 1] == 0.0f);

    CHECK(rig.publish(rig.plan({}, {{0, 0, 1}}, 2, order, pool, 0.0f)));
    const int64_t at = rig.frame;
    rig.block();
    CHECK(rig.out[1][0] < 0.05f * signal(at));
    CHECK(rig.out[1][kBlock - 1] == signal(at + kBlock - 1));
    for (uint32_t i = 1; i < kBlock; ++i) {
        const float step = rig.out[1][i] - rig.out[1][i - 1];
        CHECK(step < 0.1f && step > -0.1f);
    }
    // Back to bus 0: it too comes in from silence.
    CHECK(rig.publish(rig.plan({}, {{0, 0, 0}}, 2, order, pool, 0.0f)));
    const int64_t back = rig.frame;
    rig.block();
    CHECK(rig.out[0][0] < 0.05f * signal(back));
    CHECK(rig.out[0][kBlock - 1] == signal(back + kBlock - 1));
    CHECK(heap_untouched());
    return 0;
}

// #163 review, finding 2: a track's first send splits its TrackOut into
// TrackSource -> TrackFader, and removing its last joins it again. With a
// latent insert elsewhere the dry track's edge into the bus is delayed;
// the split must keep that delay line, or the dry path drops out for the
// length of the compensation.
int test_first_send_keeps_the_dry_delay_line() {
    Rig rig(2);
    Session& s = *rig.session;
    DelayRunner runner(1, 4096);
    s.set_insert_chains(&runner);
    s.tracks()[0]->set_pan(-1.0f);
    s.tracks()[1]->set_gain(0.0f);   // only there for its latency
    s.set_send(0, 0, 1.0f, 0.0f, false, false, true);
    s.set_aux(0, 1.0f, 0.0f, true);   // the wet is not under test
    const ChainBinding none;
    const ChainBinding c = latent_chain(0, 100);
    DelayLinePool pool(s.delay_lines());
    const std::vector<uint32_t> order{0};
    const auto dry = [&] { return rig.plan({none, c}, {}, 1, order, pool); };
    const auto sent = [&] { return rig.plan({none, c}, {{0, 0, 0}}, 1, order, pool); };
    CHECK(rig.publish(dry()));
    reset_heap_counts();
    for (int i = 0; i < 4; ++i) rig.block();
    long checked = 0;
    const auto steady = [&](int blocks) {
        for (int b = 0; b < blocks; ++b) {
            const int64_t at = rig.frame;
            rig.block();
            for (uint32_t i = 0; i < kBlock; ++i) {
                if (rig.out[0][i] != signal(at + i - 100) || rig.out[1][i] != 0.0f) {
                    std::fprintf(stderr, "  frame %lld: L %g want %g\n",
                                 static_cast<long long>(at + i), rig.out[0][i],
                                 signal(at + i - 100));
                    return false;
                }
                ++checked;
            }
        }
        return true;
    };
    CHECK(steady(1));
    CHECK(rig.publish(sent()));
    CHECK(steady(4));
    CHECK(rig.publish(dry()));
    CHECK(steady(4));
    CHECK(checked == 9 * kBlock);
    CHECK(heap_untouched());
    return 0;
}

// #163 review, finding 4: a send kept across a plan swap, the plan
// republished by an edit elsewhere (a new, empty bus), goes on as if no
// swap happened: its ramp, mid-ride on the swap's callback, and its delay
// line, behind a 100-frame chain on the other track.
int test_kept_send_survives_a_swap() {
    std::vector<float> runs[2][2];
    for (int r = 0; r < 2; ++r) {
        Rig rig(2);
        Session& s = *rig.session;
        DelayRunner runner(1, 4096);
        s.set_insert_chains(&runner);
        s.tracks()[0]->set_pan(-0.5f);
        s.tracks()[1]->set_pan(0.25f);
        s.set_send(0, 0, 0.5f, 0.0f, false, false, true);
        s.set_send(1, 3, 0.75f, -0.25f, false, false, false);
        s.set_aux(0, 0.8f, 0.1f, false);
        const ChainBinding c = latent_chain(0, 100);
        DelayLinePool pool(s.delay_lines());
        const std::vector<uint32_t> one{0};
        const std::vector<uint32_t> two{0, 1};
        const std::vector<SendBinding> sends{{0, 0, 0}, {1, 3, 0}};
        PlaybackPlan* p = rig.plan({c}, sends, 1, one, pool);
        // The dry track's send is delayed to meet the chained one's.
        bool send_delayed = false;
        for (uint32_t e = 0; e < p->edge_count; ++e) {
            if (p->edges[e].send == 1 * sonora::kStripSendSlots + 3) {
                send_delayed = p->edges[e].delay_frames == 100 &&
                               p->edges[e].delay_line != sonora::kNoDelayLine;
            }
        }
        CHECK(send_delayed);
        CHECK(rig.publish(p));
        reset_heap_counts();
        for (int blk = 0; blk < 10; ++blk) {
            if (blk == 5) {
                s.set_send(0, 0, 0.125f, 0.0f, false, false, true);
                s.set_send(1, 3, 0.25f, 0.5f, false, false, false);
                if (r == 1) CHECK(rig.publish(rig.plan({c}, sends, 2, two, pool)));
            }
            rig.block();
            for (int o = 0; o < 2; ++o) {
                runs[r][o].insert(runs[r][o].end(), rig.out[o].begin(),
                                  rig.out[o].begin() + kBlock);
            }
        }
        CHECK(heap_untouched());
    }
    CHECK(runs[0][0] == runs[1][0]);
    CHECK(runs[0][1] == runs[1][1]);
    return 0;
}

// #163 review, finding 6: the PDC line budget with sends. Dry edges into
// the bus are served before send edges, and a return with no send into it
// carries silence, which needs no line. Two lines, three tracks: track 0
// behind a chain, tracks 1 and 2 dry, all three sending to bus 0; bus 1
// empty. Four edges need a delay (two dry, two sends) and the empty return
// would be a fifth: the dry ones get the lines.
int test_dry_edges_come_first_for_lines() {
    Rig rig(3, 256, 4, 2);
    Session& s = *rig.session;
    CHECK(s.delay_lines() == 2);
    const ChainBinding c = latent_chain(0, 100);
    DelayLinePool pool(s.delay_lines());
    const std::vector<uint32_t> order{0, 1};
    PlaybackPlan* p = rig.plan({c}, {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}}, 2, order, pool);
    uint32_t bus = sonora::kNoNode;
    for (uint32_t n = 0; n < p->node_count; ++n) {
        if (p->nodes[n].kind == NodeKind::BusSum) bus = n;
    }
    CHECK(bus != sonora::kNoNode);
    const PlanNode& sum = p->nodes[bus];
    for (uint32_t k = 0; k < sum.input_edge_count; ++k) {
        const sonora::PlanEdge& e = p->edges[sum.first_input_edge + k];
        const PlanNode& from = p->nodes[e.from_node];
        if (from.kind == NodeKind::TrackFader && (from.track == 1 || from.track == 2)) {
            CHECK(e.delay_frames == 100 && e.delay_line != sonora::kNoDelayLine);
        }
        if (from.kind == NodeKind::AuxFader && from.track == 1) {
            CHECK(e.delay_frames == 0 && e.delay_line == sonora::kNoDelayLine);
        }
    }
    CHECK(p->undelayed_edges == 2);
    sonora::destroy_plan(p);
    return 0;
}

// Measured for the same finding: at the default sizes the dry mix always
// fits. 18 tracks (the UMC1820's inputs) and 32 buses are 50 edges into the
// bus, under the 128 lines, however many sends there are. Every track sends
// in all ten slots here, so the sends ask for more lines than are left.
int test_dry_mix_fits_at_default_sizes() {
    Rig big(18, 256, 32);
    CHECK(big.session->delay_lines() == 128);
    DelayLinePool big_pool(big.session->delay_lines());
    std::vector<uint32_t> all(32);
    for (uint32_t b = 0; b < 32; ++b) all[b] = b;
    std::vector<SendBinding> many;
    for (uint32_t t = 0; t < 18; ++t) {
        for (uint32_t slot = 0; slot < sonora::kStripSendSlots; ++slot) {
            many.push_back({t, slot, (t * 3 + slot) % 32});
        }
    }
    std::vector<ChainBinding> chains(18);
    for (uint32_t t = 0; t < 18; t += 2) chains[t] = latent_chain(t, 10 * (t + 1));
    PlaybackPlan* q = big.plan(chains, many, 32, all, big_pool);
    CHECK(q->undelayed_edges > 0);   // sends played early, and reported
    CHECK(q->compensation_shortfall_frames > 0);
    // The same plan with lines to spare says what each dry edge needs.
    DelayLinePool spare(1024);
    PlaybackPlan* full = big.plan(chains, many, 32, all, spare);
    CHECK(full->undelayed_edges == 0);
    uint32_t delayed = 0;
    for (uint32_t n = 0; n < q->node_count; ++n) {
        if (q->nodes[n].kind != NodeKind::BusSum) continue;
        for (uint32_t k = 0; k < q->nodes[n].input_edge_count; ++k) {
            const uint32_t e = q->nodes[n].first_input_edge + k;
            CHECK(q->edges[e].delay_frames == full->edges[e].delay_frames);
            if (q->edges[e].delay_frames > 0) ++delayed;
        }
    }
    CHECK(delayed > 0);
    sonora::destroy_plan(q);
    sonora::destroy_plan(full);
    return 0;
}

// A plan naming a send state past the session's is refused when adopted:
// silence, counted, never an index followed.
int test_send_out_of_range_is_refused() {
    Rig rig(1, 256, 1);
    DelayLinePool pool(rig.session->delay_lines());
    const std::vector<uint32_t> order{0};
    PlaybackPlan* p = rig.plan({}, {{0, 0, 0}}, 1, order, pool);
    for (uint32_t e = 0; e < p->edge_count; ++e) {
        if (p->edges[e].send != sonora::kNoSend) p->edges[e].send = 999;
    }
    CHECK(rig.publish(p));
    const uint64_t before = rig.session->invalid_plan_blocks();
    rig.block();
    CHECK(rig.session->invalid_plan_blocks() == before + 1);

    // And an aux node past max_buses.
    PlaybackPlan* q = rig.plan({}, {{0, 0, 0}}, 1, order, pool);
    for (uint32_t n = 0; n < q->node_count; ++n) {
        if (q->nodes[n].kind == NodeKind::AuxFader) q->nodes[n].track = 1;
    }
    CHECK(rig.publish(q));
    rig.block();
    CHECK(rig.session->invalid_plan_blocks() == before + 2);
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
    t(test_post_and_pre_fader);
    t(test_bus_into_bus_and_order);
    t(test_block_split_invariance);
    t(test_new_send_ramps_in);
    t(test_sends_are_compensated);
    t(test_send_out_of_range_is_refused);
    t(test_reassigned_send_ramps_in);
    t(test_first_send_keeps_the_dry_delay_line);
    t(test_kept_send_survives_a_swap);
    t(test_dry_edges_come_first_for_lines);
    t(test_dry_mix_fits_at_default_sizes);
    if (failed != 0) {
        std::fprintf(stderr, "sends: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("sends: %d passed (heap counter: %s)\n", run, playback_test::heap_counter_scope());
    return 0;
}
