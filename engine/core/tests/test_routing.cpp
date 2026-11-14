// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output routing and aux solo through Session::process() (ADR-0024): a strip
// feeding an aux bus instead of the Master, a bus into a bus, PDC over the
// routed paths, a reroute at a plan swap, an output the layout cannot place
// falling back to the Master, aux solo reaching what feeds it, the same
// output whatever the block split, and no heap touched on the audio thread.
#include <cstdio>
#include <memory>
#include <vector>

#define PLAYBACK_TEST_COUNT_HEAP 1
#include "playback_test.h"
#include "sonora/insert_chain_runner.h"
#include "sonora/plan_graph.h"
#include "sonora/session.h"

// The solo hook is private to Session; only a test defines this.
namespace sonora {
struct SessionTestAccess {
    static void set_solo_hook(Session& s, Session::WordHook hook, void* context) {
        s.solo_hook_ = hook;
        s.solo_hook_context_ = context;
    }
};
}  // namespace sonora

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
    // Six device outputs: the Master on 1-2, an output path on 3-4 or 5-6.
    static constexpr uint32_t kOutputs = 6;
    std::vector<float> out[kOutputs];
    float* out_ptrs[kOutputs];
    int64_t frame = 0;

    explicit Rig(int tracks, uint32_t max_block = 256, uint32_t max_buses = 4,
                 uint32_t max_delayed_edges = 128) {
        sonora::SessionConfig cfg;
        cfg.dir = "test_routing_unused";
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
        for (uint32_t o = 0; o < kOutputs; ++o) {
            out[o].assign(kMaxCallback, 0.0f);
            out_ptrs[o] = out[o].data();
        }
        // Every track hears its input: the graph is under test.
        for (auto& t : session->tracks()) t->set_monitor(sonora::Track::Monitor::Input);
    }

    // A plan with no regions, the given chains, sends and buses, laid out
    // and compensated as engine/control's builder does.
    // `pan_exponent` 0 makes a mono pan at centre 1 on each side, for sums
    // that are exact. Outputs are engine bus indexes, sonora::kNoBus the Master.
    PlaybackPlan* plan(const std::vector<ChainBinding>& chains, const std::vector<SendBinding>& sends,
                       uint32_t buses, const std::vector<uint32_t>& order, DelayLinePool& pool,
                       std::vector<uint32_t> track_out = {}, std::vector<uint32_t> bus_out = {},
                       float pan_exponent = 0.0f, std::vector<uint16_t> bus_hw = {}) {
        const auto tracks = static_cast<uint32_t>(session->tracks().size());
        std::vector<ChainBinding> c = chains;
        c.resize(tracks);
        track_out.resize(tracks, sonora::kNoBus);
        bus_out.resize(buses, sonora::kNoBus);
        MixLayout mix;
        mix.track_outputs = track_out.data();
        mix.bus_outputs = bus_out.data();
        bus_hw.resize(buses, sonora::kNoHardware);
        mix.bus_hardware = bus_hw.data();
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
        b.output_count = kOutputs;
        b.nframes = n;
        b.callback_ns = 1'000'000'000 + static_cast<uint64_t>(frame) * 1000;
        t_counting = true;
        session->process(b);
        t_counting = false;
        frame += n;
    }
};

constexpr uint32_t kNoBus = sonora::kNoBus;

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

// Plays two blocks to settle ramps, then checks the third against
// left(x), right(x) per frame.
template <typename F0, typename F1>
bool heard(Rig& rig, F0 left, F1 right) {
    rig.block();
    rig.block();
    const int64_t at = rig.frame;
    rig.block();
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float x = signal(at + i);
        if (rig.out[0][i] != left(x) || rig.out[1][i] != right(x)) {
            std::fprintf(stderr, "  frame %u: L %g want %g, R %g want %g\n", i, rig.out[0][i],
                         left(x), rig.out[1][i], right(x));
            return false;
        }
    }
    return true;
}

// Two tracks into a bus, a third straight to the Master: the bus sums what
// is routed to it, then its gain; the Master sums the third and the return.
int test_tracks_to_a_bus() {
    Rig rig(3);
    Session& s = *rig.session;
    s.tracks()[0]->set_gain(0.5f);
    s.tracks()[1]->set_gain(0.25f);
    s.tracks()[2]->set_gain(0.125f);
    s.set_aux(0, 2.0f, 0.0f, false);
    DelayLinePool pool(s.delay_lines());
    const std::vector<uint32_t> order{0};
    PlaybackPlan* p = rig.plan({}, {}, 1, order, pool, {0, 0, kNoBus});
    // Three tracks, the bus's sum and fader, the Master's sum and out; each
    // strip has exactly one edge out (ADR-0024 section 6).
    CHECK(p->node_count == 3 + 2 + 2);
    CHECK(p->edge_count == 6);
    const PlanNode& sum = p->nodes[3];
    CHECK(sum.kind == NodeKind::AuxSum && sum.input_edge_count == 2);
    CHECK(p->edges[sum.first_input_edge].send == sonora::kNoSend);
    CHECK(p->edges[sum.first_input_edge].from_node == 0);
    CHECK(p->edges[sum.first_input_edge + 1].from_node == 1);
    const PlanNode& master = p->nodes[5];
    CHECK(master.kind == NodeKind::BusSum && master.input_edge_count == 2);
    CHECK(rig.publish(p));
    reset_heap_counts();
    const auto want = [](float x) { return (x * 0.5f + x * 0.25f) * 2.0f + x * 0.125f; };
    CHECK(heard(rig, want, want));
    CHECK(heap_untouched());
    return 0;
}

// Track to bus 1, bus 1 to bus 0, bus 0 to the Master; bus 0 is listed
// first and laid out last. A bus whose output sits before it in the order
// is not laid out backwards: it feeds the Master, and the sizes agree.
int test_bus_into_bus() {
    Rig rig(1);
    Session& s = *rig.session;
    s.set_aux(0, 0.5f, 0.0f, false);
    s.set_aux(1, 2.0f, 0.0f, false);
    DelayLinePool pool(s.delay_lines());
    const std::vector<uint32_t> order{1, 0};
    CHECK(rig.publish(rig.plan({}, {}, 2, order, pool, {1}, {kNoBus, 0})));
    reset_heap_counts();
    const auto want = [](float x) { return x * 2.0f * 0.5f; };
    CHECK(heard(rig, want, want));
    CHECK(heap_untouched());

    const std::vector<uint32_t> backwards{0, 1};
    PlaybackPlan* q = rig.plan({}, {}, 2, backwards, pool, {1}, {kNoBus, 0});
    // Bus 1 comes after bus 0 here, so its output (bus 0) is behind it: the
    // Master. The track's, bus 1, is ahead of it and stands.
    const PlanNode& master = q->nodes[q->node_count - 2];
    CHECK(master.kind == NodeKind::BusSum && master.input_edge_count == 2);
    sonora::destroy_plan(q);
    return 0;
}

// PDC over routed paths: a track behind a 100-frame chain, routed to a bus,
// and a dry track on the Master meet aligned at the output.
int test_routed_paths_are_compensated() {
    Rig rig(2);
    Session& s = *rig.session;
    DelayRunner runner(1, 4096);
    s.set_insert_chains(&runner);
    s.tracks()[0]->set_pan(-1.0f);
    s.tracks()[1]->set_pan(-1.0f);
    s.set_aux(0, 1.0f, 0.0f, false);
    DelayLinePool pool(s.delay_lines());
    const std::vector<uint32_t> order{0};
    PlaybackPlan* p = rig.plan({latent_chain(0, 100)}, {}, 1, order, pool, {0, kNoBus});
    CHECK(p->compensation_frames == 100);
    CHECK(p->compensation_shortfall_frames == 0);
    CHECK(rig.publish(p));
    reset_heap_counts();
    for (int i = 0; i < 4; ++i) rig.block();
    for (int blk = 0; blk < 6; ++blk) {
        const int64_t at = rig.frame;
        rig.block();
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float want = signal(at + i - 100);
            CHECK(rig.out[0][i] == want + want);
        }
    }
    CHECK(heap_untouched());
    return 0;
}

// A reroute at a swap: the track plays on the Master, then through a bus
// with its own gain, then back; the output settles to each route's level.
int test_reroute_at_a_swap() {
    Rig rig(1);
    Session& s = *rig.session;
    s.set_aux(0, 0.5f, 0.0f, false);
    DelayLinePool pool(s.delay_lines());
    const std::vector<uint32_t> order{0};
    const auto full = [](float x) { return x; };
    const auto half = [](float x) { return x * 0.5f; };
    CHECK(rig.publish(rig.plan({}, {}, 1, order, pool, {kNoBus})));
    reset_heap_counts();
    CHECK(heard(rig, full, full));
    CHECK(rig.publish(rig.plan({}, {}, 1, order, pool, {0})));
    CHECK(heard(rig, half, half));
    CHECK(rig.publish(rig.plan({}, {}, 1, order, pool, {kNoBus})));
    CHECK(heard(rig, full, full));
    CHECK(heap_untouched());
    return 0;
}

// Aux solo (ADR-0024 section 4): soloing a bus plays what feeds it, directly,
// by a send, or through another bus, and mutes the rest; solo safe stays.
int test_aux_solo() {
    Rig rig(5);
    Session& s = *rig.session;
    // 0 -> bus 0; 1 on the Master; 2 sends to bus 0; 3 -> bus 1 -> bus 0;
    // 4 solo safe on the Master.
    s.tracks()[0]->set_gain(0.5f);
    s.tracks()[1]->set_gain(0.25f);
    s.tracks()[2]->set_gain(0.0f);   // the send is pre-fader: only it is heard
    s.tracks()[3]->set_gain(0.125f);
    s.tracks()[4]->set_gain(1.0f / 16.0f);
    s.tracks()[4]->set_solo_safe(true);
    s.set_send(2, 0, 1.0f, 0.0f, false, false, true);
    DelayLinePool pool(s.delay_lines());
    const std::vector<uint32_t> order{1, 0};
    CHECK(rig.publish(rig.plan({}, {{2, 0, 0}}, 2, order, pool, {0, kNoBus, kNoBus, 1, kNoBus},
                               {kNoBus, 0})));
    reset_heap_counts();
    const auto all = [](float x) { return x * (0.5f + 1.0f + 0.125f) + x * 0.25f + x / 16.0f; };
    CHECK(heard(rig, all, all));

    // Bus 0 alone lets in 0, 2 and 3 (through bus 1), and the solo-safe
    // track; track 1 is muted.
    s.set_aux(0, 1.0f, 0.0f, false, true);
    const auto solo0 = [](float x) { return x * (0.5f + 1.0f + 0.125f) + x / 16.0f; };
    CHECK(heard(rig, solo0, solo0));

    // Bus 1 alone: track 3 and the solo-safe track.
    s.set_aux(0, 1.0f, 0.0f, false, false);
    s.set_aux(1, 1.0f, 0.0f, false, true);
    const auto solo1 = [](float x) { return x * 0.125f + x / 16.0f; };
    CHECK(heard(rig, solo1, solo1));

    // A track soloed beside a soloed bus: both play.
    s.tracks()[1]->set_soloed(true);
    const auto both = [](float x) { return x * 0.125f + x * 0.25f + x / 16.0f; };
    CHECK(heard(rig, both, both));
    s.tracks()[1]->set_soloed(false);
    s.set_aux(1, 1.0f, 0.0f, false, false);
    CHECK(heard(rig, all, all));
    CHECK(heap_untouched());
    return 0;
}

// Implicit solo follows the plan being played: rerouting the soloed bus's
// source away from it mutes the source with the plan that swaps in.
int test_aux_solo_follows_the_plan() {
    Rig rig(2);
    Session& s = *rig.session;
    s.tracks()[0]->set_gain(0.5f);
    s.tracks()[1]->set_gain(0.25f);
    s.set_aux(0, 1.0f, 0.0f, false, true);
    DelayLinePool pool(s.delay_lines());
    const std::vector<uint32_t> order{0};
    CHECK(rig.publish(rig.plan({}, {}, 1, order, pool, {0, kNoBus})));
    reset_heap_counts();
    const auto first = [](float x) { return x * 0.5f; };
    CHECK(heard(rig, first, first));
    CHECK(rig.publish(rig.plan({}, {}, 1, order, pool, {kNoBus, 0})));
    const auto second = [](float x) { return x * 0.25f; };
    CHECK(heard(rig, second, second));
    CHECK(heap_untouched());
    return 0;
}

// The control thread clears a bus's solo once the callback has loaded the
// solo flags (ADR-0148 §1).
struct SoloClear {
    Session* session = nullptr;
    bool armed = false;
};

void clear_solo_once(void* context) noexcept SONORA_RT {
    auto* c = static_cast<SoloClear*>(context);
    if (!c->armed) return;
    c->armed = false;
    c->session->set_aux_solo(0, false);
}

// A solo cleared between two loads of the flags in one callback: the
// callback plays one view of them, so the track feeding the soloed bus is
// not muted for a block with every other unsoloed track.
int test_solo_cleared_mid_callback_mutes_nothing() {
    Rig rig(2);
    Session& s = *rig.session;
    s.tracks()[0]->set_gain(0.5f);
    s.tracks()[1]->set_gain(0.25f);
    s.set_aux(0, 1.0f, 0.0f, false, true);
    DelayLinePool pool(s.delay_lines());
    const std::vector<uint32_t> order{0};
    CHECK(rig.publish(rig.plan({}, {}, 1, order, pool, {0, kNoBus})));
    reset_heap_counts();
    const auto soloed = [](float x) { return x * 0.5f; };
    CHECK(heard(rig, soloed, soloed));

    SoloClear clear{&s, true};
    sonora::SessionTestAccess::set_solo_hook(s, clear_solo_once, &clear);
    const int64_t at = rig.frame;
    rig.block();
    sonora::SessionTestAccess::set_solo_hook(s, nullptr, nullptr);
    CHECK(!clear.armed);
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float want = signal(at + i) * 0.5f;
        if (rig.out[0][i] != want || rig.out[1][i] != want) {
            std::fprintf(stderr, "  frame %u: L %g R %g want %g\n", i, rig.out[0][i], rig.out[1][i],
                         want);
            return 1;
        }
    }
    const auto all = [](float x) { return x * 0.5f + x * 0.25f; };
    CHECK(heard(rig, all, all));
    CHECK(heap_untouched());
    return 0;
}

// The edge from bus `b`'s return into the Master.
const sonora::PlanEdge* return_edge(const PlaybackPlan& p, uint32_t b) {
    for (uint32_t ni = 0; ni < p.node_count; ++ni) {
        if (p.nodes[ni].kind != NodeKind::BusSum) continue;
        for (uint32_t k = 0; k < p.nodes[ni].input_edge_count; ++k) {
            const sonora::PlanEdge& e = p.edges[p.nodes[ni].first_input_edge + k];
            const PlanNode& from = p.nodes[e.from_node];
            if (from.kind == NodeKind::AuxFader && from.track == b) return &e;
        }
    }
    return nullptr;
}

// The swap that removes a bus's last send leaves the send's last frames in
// the return's delay line: the line is kept, aligned, and plays them out
// rather than cutting them (ADR-0148 §2). A bus that never carried sound
// still takes no line.
int test_removed_send_plays_out_of_the_return_line() {
    Rig rig(2);
    Session& s = *rig.session;
    DelayRunner runner(1, 4096);
    s.set_insert_chains(&runner);
    s.tracks()[0]->set_gain(0.0f);   // only its 100-frame chain counts
    s.tracks()[1]->set_gain(0.0f);   // only its pre-fader send is heard
    s.set_send(1, 0, 1.0f, 0.0f, false, false, true);
    s.set_aux(0, 1.0f, 0.0f, false);
    DelayLinePool pool(s.delay_lines());
    const std::vector<uint32_t> order{0};
    PlaybackPlan* p = rig.plan({latent_chain(0, 100)}, {{1, 0, 0}}, 1, order, pool);
    const sonora::PlanEdge* before = return_edge(*p, 0);
    CHECK(before != nullptr && before->delay_frames == 100);
    const uint32_t line = before->delay_line;
    CHECK(line < s.delay_lines());
    CHECK(rig.publish(p));
    reset_heap_counts();
    for (int i = 0; i < 4; ++i) rig.block();
    int64_t at = rig.frame;
    rig.block();
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(rig.out[0][i] == signal(at + i - 100));

    PlaybackPlan* q = rig.plan({latent_chain(0, 100)}, {}, 1, order, pool);
    const sonora::PlanEdge* after = return_edge(*q, 0);
    CHECK(after != nullptr && after->delay_frames == 100 && after->delay_line == line);
    CHECK(q->compensation_shortfall_frames == 0 && q->undelayed_edges == 0);
    const int64_t cut = rig.frame;
    CHECK(rig.publish(q));
    for (int blk = 0; blk < 3; ++blk) {
        at = rig.frame;
        rig.block();
        for (uint32_t i = 0; i < kBlock; ++i) {
            const int64_t t = at + i;
            const float want = t < cut + 100 ? signal(t - 100) : 0.0f;
            if (rig.out[0][i] != want) {
                std::fprintf(stderr, "  frame %lld: %g want %g\n", static_cast<long long>(t),
                             rig.out[0][i], want);
                return 1;
            }
        }
    }
    CHECK(heap_untouched());

    DelayLinePool fresh(s.delay_lines());
    PlaybackPlan* r = rig.plan({latent_chain(0, 100)}, {}, 1, order, fresh);
    const sonora::PlanEdge* silent = return_edge(*r, 0);
    CHECK(silent != nullptr && silent->delay_frames == 0 &&
          silent->delay_line == sonora::kNoDelayLine);
    sonora::destroy_plan(r);
    return 0;
}

// The output does not depend on how a callback is split into max_block
// pieces, with routing, aux solo and a reroute mid-run.
int test_block_split_invariance() {
    std::vector<float> runs[2][2];
    const uint32_t pieces[2] = {256, 32};
    for (int r = 0; r < 2; ++r) {
        Rig rig(3, pieces[r]);
        Session& s = *rig.session;
        s.tracks()[0]->set_pan(-0.3f);
        s.tracks()[1]->set_gain(0.7f);
        s.set_aux(0, 0.9f, 0.2f, false);
        s.set_aux(1, 0.6f, -0.7f, false);
        DelayLinePool pool(s.delay_lines());
        const std::vector<uint32_t> order{0, 1};
        CHECK(rig.publish(rig.plan({}, {}, 2, order, pool, {0, 0, 1}, {1, kNoBus}, 0.5f)));
        reset_heap_counts();
        for (int blk = 0; blk < 8; ++blk) {
            if (blk == 2) s.set_aux(0, 0.3f, -0.4f, false, true);
            if (blk == 4) {
                CHECK(rig.publish(rig.plan({}, {}, 2, order, pool, {0, 1, 1}, {1, kNoBus}, 0.5f)));
            }
            if (blk == 6) s.set_aux(0, 0.3f, -0.4f, false, false);
            rig.block(256);
            for (int o = 0; o < 2; ++o) {
                runs[r][o].insert(runs[r][o].end(), rig.out[o].begin(), rig.out[o].begin() + 256);
            }
        }
        CHECK(heap_untouched());
    }
    CHECK(runs[0][0] == runs[1][0]);
    CHECK(runs[0][1] == runs[1][1]);
    return 0;
}

// ADR-0060: a bus that is an output path plays on its own device pair,
// not on the Master, and the Master does not hear it.
int test_output_path_plays_on_its_pair() {
    Rig rig(2);
    Session& s = *rig.session;
    s.tracks()[0]->set_gain(0.5f);
    s.tracks()[1]->set_gain(0.25f);
    s.set_aux(0, 2.0f, 0.0f, false);
    DelayLinePool pool(s.delay_lines());
    const std::vector<uint32_t> order{0};
    PlaybackPlan* p = rig.plan({}, {}, 1, order, pool, {0, kNoBus}, {}, 0.0f, {2});
    // Two tracks, the bus's sum and fader, the Master's sum and out, and the
    // path's sink after them.
    CHECK(p->node_count == 2 + 2 + 2 + 1);
    const PlanNode& sink = p->nodes[p->node_count - 1];
    CHECK(sink.kind == NodeKind::PathOut && sink.hardware_out == 2 && sink.input_edge_count == 1);
    CHECK(p->nodes[p->edges[sink.first_input_edge].from_node].kind == NodeKind::AuxFader);
    const PlanNode& master = p->nodes[p->node_count - 3];
    CHECK(master.kind == NodeKind::BusSum && master.input_edge_count == 1);
    CHECK(rig.publish(p));
    reset_heap_counts();
    for (int i = 0; i < 3; ++i) rig.block();
    const int64_t at = rig.frame;
    rig.block();
    for (uint32_t i = 0; i < kBlock; ++i) {
        const float x = signal(at + i);
        CHECK(rig.out[0][i] == x * 0.25f);   // the Master: track 1 only
        CHECK(rig.out[1][i] == rig.out[0][i]);
        CHECK(rig.out[2][i] == x * 0.5f * 2.0f);   // the path: track 0 through the bus
        CHECK(rig.out[3][i] == rig.out[2][i]);
        CHECK(rig.out[4][i] == 0.0f && rig.out[5][i] == 0.0f);
    }
    CHECK(heap_untouched());
    return 0;
}

// The path and the Master play the same moment (ADR-0060): a chain on the
// Master's side delays the path's dry track, and the other way round.
int test_output_path_is_aligned_with_the_master() {
    for (int chain_on_path = 0; chain_on_path < 2; ++chain_on_path) {
        Rig rig(2);
        Session& s = *rig.session;
        DelayRunner runner(1, 4096);
        s.set_insert_chains(&runner);
        s.tracks()[0]->set_pan(-1.0f);
        s.tracks()[1]->set_pan(-1.0f);
        s.set_aux(0, 1.0f, 0.0f, false);
        DelayLinePool pool(s.delay_lines());
        const std::vector<uint32_t> order{0};
        // Track 0 plays on the path, track 1 on the Master.
        std::vector<ChainBinding> chains(2);
        chains[chain_on_path ? 0 : 1] = latent_chain(0, 100);
        PlaybackPlan* p = rig.plan(chains, {}, 1, order, pool, {0, kNoBus}, {}, 0.0f, {4});
        CHECK(p->compensation_frames == 100);
        CHECK(p->compensation_shortfall_frames == 0);
        CHECK(rig.publish(p));
        reset_heap_counts();
        for (int i = 0; i < 4; ++i) rig.block();
        for (int blk = 0; blk < 4; ++blk) {
            const int64_t at = rig.frame;
            rig.block();
            for (uint32_t i = 0; i < kBlock; ++i) {
                const float want = signal(at + i - 100);
                CHECK(rig.out[0][i] == want);
                CHECK(rig.out[4][i] == want);
            }
        }
        CHECK(heap_untouched());
    }
    return 0;
}

// A path whose pair the device does not have is played nowhere, and the
// rest of the mix is untouched.
int test_output_path_past_the_device_is_dropped() {
    Rig rig(2);
    Session& s = *rig.session;
    s.tracks()[0]->set_pan(-1.0f);
    s.tracks()[1]->set_pan(-1.0f);
    s.set_aux(0, 1.0f, 0.0f, false);
    DelayLinePool pool(s.delay_lines());
    const std::vector<uint32_t> order{0};
    CHECK(rig.publish(rig.plan({}, {}, 1, order, pool, {0, kNoBus}, {}, 0.0f, {Rig::kOutputs})));
    for (int i = 0; i < 3; ++i) rig.block();
    const int64_t at = rig.frame;
    rig.block();
    for (uint32_t i = 0; i < kBlock; ++i) {
        CHECK(rig.out[0][i] == signal(at + i));
        CHECK(rig.out[2][i] == 0.0f && rig.out[5][i] == 0.0f);
    }
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
    t(test_tracks_to_a_bus);
    t(test_bus_into_bus);
    t(test_routed_paths_are_compensated);
    t(test_reroute_at_a_swap);
    t(test_aux_solo);
    t(test_aux_solo_follows_the_plan);
    t(test_solo_cleared_mid_callback_mutes_nothing);
    t(test_removed_send_plays_out_of_the_return_line);
    t(test_block_split_invariance);
    t(test_output_path_plays_on_its_pair);
    t(test_output_path_is_aligned_with_the_master);
    t(test_output_path_past_the_device_is_dropped);
    if (failed != 0) {
        std::fprintf(stderr, "routing: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("routing: %d passed (heap counter: %s)\n", run, playback_test::heap_counter_scope());
    return 0;
}
