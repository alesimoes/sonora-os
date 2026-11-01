// SPDX-License-Identifier: GPL-3.0-or-later
//
// Plugin parameter lanes on the audio thread (ADR-0027 §5): an InsertChain
// node carries lanes, each naming an instance of its chain and a parameter
// as ParamEvent does, its points in the plan's auto_points. Once per
// callback that runs the chain the session evaluates each lane at the
// callback's first frame, the frame the chain's input carries, and hands
// the value to the node as a parameter change at offset 0, after the
// changes the control thread posted: the plugin_client store and its
// last-value table take it from there, so there is no new IPC.
//
//   follows_the_lane   stopped, the lane's value at the playhead, once;
//                      rolling, the lane at each callback's first frame,
//                      sent only when it moves; nothing allocated
//   lane_wins          a posted value for an automated node is followed,
//                      in the same callback, by its lanes' values
//   modes              Off and Write play no lane; back to Read resends
//   new_plan           a plan swap resends every lane once
//   bounds             a lane out of range, on a node that is not a chain,
//                      or past PlaybackConfig::max_plugin_lanes: the plan is
//                      refused, never followed
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#define PLAYBACK_TEST_COUNT_HEAP 1
#include "playback_test.h"
#include "sonora/insert_chain_runner.h"
#include "sonora/plan_graph.h"
#include "sonora/session.h"

using playback_test::heap_untouched;
using playback_test::reset_heap_counts;
using playback_test::t_counting;

namespace {

using sonora::ChainBinding;
using sonora::ChainParamEvent;
using sonora::DelayLinePool;
using sonora::PlanAutoPoint;
using sonora::PlanNode;
using sonora::PlaybackPlan;
using sonora::Session;
using sonora::Track;

constexpr uint32_t kBlock = 64;
constexpr uint32_t kMaxLanes = 4;

struct Seen {
    uint64_t callback;
    ChainParamEvent event;
};

class RecordingRunner final : public sonora::InsertChainRunner {
public:
    void begin(uint64_t) noexcept SONORA_RT override { ++callback; }
    void end() noexcept SONORA_RT override {}
    uint32_t block_frames() const noexcept SONORA_RT override { return 0; }
    void run(const PlanNode&, const float* const in[2], float* const out[2], uint32_t frames,
             const ChainParamEvent* events, uint32_t event_count) noexcept SONORA_RT override {
        for (uint32_t i = 0; i < frames; ++i) {
            out[0][i] = in[0][i];
            out[1][i] = in[1][i];
        }
        for (uint32_t i = 0; i < event_count && count < seen.size(); ++i)
            seen[count++] = Seen{callback, events[i]};
    }
    void skip(const PlanNode&, float* const out[2], uint32_t frames) noexcept SONORA_RT override {
        for (uint32_t i = 0; i < frames; ++i) out[0][i] = out[1][i] = 0.0f;
    }
    std::vector<Seen> seen = std::vector<Seen>(512);
    std::size_t count = 0;
    uint64_t callback = 0;
};

struct LaneSpec {
    uint32_t track;
    uint32_t instance;
    uint32_t param;
    std::vector<PlanAutoPoint> points;
};

struct Rig {
    std::unique_ptr<Session> session;
    std::vector<std::vector<float>> in;
    std::vector<const float*> in_ptrs;
    std::vector<float> out[2];
    float* out_ptrs[2];
    RecordingRunner runner;
    DelayLinePool pool;

    explicit Rig(int tracks) {
        sonora::SessionConfig cfg;
        cfg.dir = "test_plugin_automation_unused";
        cfg.track_count = tracks;
        cfg.ring_seconds = 0.1;
        cfg.playback.chunk_frames = 256;
        cfg.playback.window_seconds = 0.02;
        cfg.playback.pool_chunks = 64;
        cfg.playback.max_block_frames = 256;
        cfg.playback.max_delay_frames = 1024;
        cfg.playback.plugin_param_capacity = 16;
        cfg.playback.max_param_stores = 32;
        cfg.playback.max_plugin_lanes = kMaxLanes;
        cfg.playback.command_capacity = 4;
        session = std::make_unique<Session>(cfg);
        pool = DelayLinePool(session->delay_lines());
        in.assign(tracks, std::vector<float>(1024, 0.0f));
        for (auto& v : in) in_ptrs.push_back(v.data());
        for (int o = 0; o < 2; ++o) {
            out[o].assign(1024, 0.0f);
            out_ptrs[o] = out[o].data();
        }
        session->set_insert_chains(&runner);
    }

    // chain_plan(), with each spec's lane on its track's InsertChain node.
    PlaybackPlan* plan(const std::vector<ChainBinding>& chains, const std::vector<LaneSpec>& lanes) {
        const auto tracks = static_cast<uint32_t>(session->tracks().size());
        uint32_t active = 0;
        for (const ChainBinding& c : chains) active += c.active ? 1 : 0;
        uint32_t points = 0;
        for (const LaneSpec& l : lanes) points += static_cast<uint32_t>(l.points.size());
        sonora::PlanSizes sizes;
        sizes.tracks = tracks;
        sizes.nodes = sonora::graph_nodes(tracks, active);
        sizes.edges = sonora::graph_edges(tracks, active);
        sizes.auto_points = points;
        sizes.plugin_lanes = static_cast<uint32_t>(lanes.size());
        PlaybackPlan* p = sonora::create_plan(sizes);
        p->epoch = session->plans().next_epoch();
        p->chunk_frames = 256;
        p->pan_exponent = 0.5f;
        for (uint32_t t = 0; t < tracks; ++t) {
            p->tracks[t].engine_track = t;
            p->tracks[t].channels = 1;
        }
        sonora::layout_track_graph(*p, chains.data());
        sonora::compensate(*p, session->max_delay_frames(), pool, nullptr);
        uint32_t at = 0;
        for (uint32_t li = 0; li < lanes.size(); ++li) {
            const LaneSpec& l = lanes[li];
            sonora::PlanPluginLane& lane = p->plugin_lanes[li];
            lane.instance = l.instance;
            lane.param = l.param;
            lane.first = at;
            lane.count = static_cast<uint32_t>(l.points.size());
            for (const PlanAutoPoint& pt : l.points) p->auto_points[at++] = pt;
            for (uint32_t ni = 0; ni < p->node_count; ++ni) {
                PlanNode& node = p->nodes[ni];
                if (node.kind != sonora::NodeKind::InsertChain || node.track != l.track) continue;
                if (node.plugin_lane_count == 0) node.first_plugin_lane = li;
                ++node.plugin_lane_count;
            }
        }
        return p;
    }

    bool publish(PlaybackPlan* p) { return playback_test::publish_plan(*session, p); }

    bool post(uint32_t store, uint32_t instance, uint32_t param, float value) {
        return session->post_plugin_param(session->plans().current()->epoch, store, instance,
                                          param, value);
    }

    void block(uint32_t n = kBlock) {
        sonora::backend::AudioBlock b{};
        b.inputs = in_ptrs.data();
        b.input_count = static_cast<uint32_t>(in_ptrs.size());
        b.outputs = out_ptrs;
        b.output_count = 2;
        b.nframes = n;
        t_counting = true;
        session->process(b);
        t_counting = false;
    }

    bool park(uint64_t frame) {
        Session& s = *session;
        if (!s.post_locate(frame)) return false;
        s.reader().run_pass();
        block();
        s.reader().run_pass();
        block();
        return s.transport().playhead() == frame;
    }
};

ChainBinding bound(uint32_t slot, uint32_t store) {
    ChainBinding c;
    c.active = true;
    c.chain_id = 100 + slot;
    c.channel_slot = slot;
    c.param_store = store;
    c.node_state = slot;
    return c;
}

// 0 at frame 0 to 1 at frame 1024, then held.
const std::vector<PlanAutoPoint> kRamp = {{0, 0.0f}, {1024, 1.0f}};

float ramp_at(uint64_t f) {
    return sonora::auto_lane_value(kRamp.data(), static_cast<uint32_t>(kRamp.size()),
                                   static_cast<int64_t>(f));
}

bool same(const ChainParamEvent& e, uint32_t instance, uint32_t param, float value) {
    return e.instance == instance && e.param == param && e.offset == 0 && e.value == value;
}

int test_follows_the_lane() {
    Rig rig(2);
    CHECK(rig.publish(rig.plan({bound(0, 5), ChainBinding{}}, {{0, 1, 3, kRamp}})));
    CHECK(rig.park(256));
    // Stopped: the lane's value at the playhead, sent once however many
    // callbacks pass.
    CHECK(rig.runner.count == 1);
    CHECK(same(rig.runner.seen[0].event, 1, 3, ramp_at(256)));
    rig.block();
    rig.block();
    CHECK(rig.runner.count == 1);

    rig.session->play();
    reset_heap_counts();
    std::size_t seen = rig.runner.count;
    float last = ramp_at(256);
    int moved = 0;
    for (int k = 0; k < 20; ++k) {
        const uint64_t at = rig.session->transport().playhead();
        rig.block();
        // One change in a callback whose first frame (the frame the chain's
        // input carries) the lane has moved at, none otherwise.
        if (ramp_at(at) != last) {
            CHECK(rig.runner.count == seen + 1);
            CHECK(same(rig.runner.seen[seen].event, 1, 3, ramp_at(at)));
            CHECK(rig.runner.seen[seen].callback == rig.runner.callback);
            last = ramp_at(at);
            ++moved;
        } else {
            CHECK(rig.runner.count == seen);
        }
        seen = rig.runner.count;
    }
    CHECK(heap_untouched());
    // Across the ramp from 256 to 1024 a block at a time, then held at 1.
    CHECK(moved == 12);
    CHECK(last == 1.0f);
    return 0;
}

int test_lane_wins() {
    Rig rig(2);
    CHECK(rig.publish(rig.plan({bound(0, 5), ChainBinding{}}, {{0, 1, 3, kRamp}})));
    CHECK(rig.park(512));
    const std::size_t was = rig.runner.count;
    // The operator turns the automated knob: the static value goes, and the
    // lane follows it in the same callback, so the host ends on the lane.
    CHECK(rig.post(5, 1, 3, 0.9f));
    reset_heap_counts();
    rig.block();
    CHECK(heap_untouched());
    CHECK(rig.runner.count == was + 2);
    CHECK(same(rig.runner.seen[was].event, 1, 3, 0.9f));
    CHECK(same(rig.runner.seen[was + 1].event, 1, 3, ramp_at(512)));
    CHECK(rig.runner.seen[was].callback == rig.runner.seen[was + 1].callback);
    return 0;
}

int test_modes() {
    Rig rig(2);
    CHECK(rig.publish(rig.plan({bound(0, 5), ChainBinding{}}, {{0, 1, 3, kRamp}})));
    Track& track = *rig.session->tracks()[0];
    track.set_automation(Track::Automation::Off);
    CHECK(rig.park(512));
    CHECK(rig.runner.count == 0);
    // Off: the knob is the operator's.
    CHECK(rig.post(5, 1, 3, 0.9f));
    rig.block();
    CHECK(rig.runner.count == 1 && same(rig.runner.seen[0].event, 1, 3, 0.9f));
    // Write plays the live value, and plugin parameters have no write yet.
    track.set_automation(Track::Automation::Write);
    rig.block();
    CHECK(rig.runner.count == 1);
    // Back to Read: the lane again, at once.
    track.set_automation(Track::Automation::Read);
    rig.block();
    CHECK(rig.runner.count == 2 && same(rig.runner.seen[1].event, 1, 3, ramp_at(512)));
    // Touch and Latch play the lane until a plugin touch exists.
    track.set_automation(Track::Automation::Touch);
    rig.block();
    track.set_automation(Track::Automation::Latch);
    rig.block();
    CHECK(rig.runner.count == 2);
    return 0;
}

int test_new_plan() {
    Rig rig(2);
    // Two lanes on one chain, one on the other track's.
    const std::vector<PlanAutoPoint> flat = {{0, 0.25f}};
    const std::vector<LaneSpec> lanes = {{0, 0, 7, flat}, {0, 2, 1, kRamp}, {1, 0, 0, flat}};
    CHECK(rig.publish(rig.plan({bound(0, 5), bound(1, 9)}, lanes)));
    CHECK(rig.park(0));
    CHECK(rig.runner.count == 3);
    CHECK(same(rig.runner.seen[0].event, 0, 7, 0.25f));
    CHECK(same(rig.runner.seen[1].event, 2, 1, 0.0f));
    CHECK(same(rig.runner.seen[2].event, 0, 0, 0.25f));
    rig.block();
    CHECK(rig.runner.count == 3);
    // A new plan may name another host: every lane goes again, once.
    CHECK(rig.publish(rig.plan({bound(0, 5), bound(1, 9)}, lanes)));
    rig.block();
    CHECK(rig.runner.count == 6);
    rig.block();
    CHECK(rig.runner.count == 6);
    return 0;
}

int test_bounds() {
    {
        Rig rig(2);
        PlaybackPlan* p = rig.plan({bound(0, 5), ChainBinding{}}, {{0, 1, 3, kRamp}});
        p->plugin_lanes[0].count = 3;   // past auto_points
        CHECK(rig.publish(p));
        rig.block();
        CHECK(rig.session->invalid_plan_blocks() == 1);
        CHECK(rig.runner.count == 0);
    }
    {
        Rig rig(2);
        PlaybackPlan* p = rig.plan({bound(0, 5), ChainBinding{}}, {{0, 1, 3, kRamp}});
        p->plugin_lanes[0].count = 0;   // a lane with no point
        CHECK(rig.publish(p));
        rig.block();
        CHECK(rig.session->invalid_plan_blocks() == 1);
    }
    {
        Rig rig(2);
        PlaybackPlan* p = rig.plan({bound(0, 5), ChainBinding{}}, {{0, 1, 3, kRamp}});
        for (uint32_t ni = 0; ni < p->node_count; ++ni) {
            PlanNode& node = p->nodes[ni];
            if (node.kind == sonora::NodeKind::InsertChain) node.first_plugin_lane = 1;
        }
        CHECK(rig.publish(p));
        rig.block();
        CHECK(rig.session->invalid_plan_blocks() == 1);
    }
    {
        Rig rig(2);
        PlaybackPlan* p = rig.plan({bound(0, 5), ChainBinding{}}, {{0, 1, 3, kRamp}});
        // A lane on a node that runs no chain.
        p->nodes[p->node_count - 1].plugin_lane_count = 1;
        CHECK(rig.publish(p));
        rig.block();
        CHECK(rig.session->invalid_plan_blocks() == 1);
    }
    {
        // Two chains naming the one lane: each would reserve it.
        Rig rig(2);
        PlaybackPlan* p = rig.plan({bound(0, 5), bound(1, 9)}, {{0, 1, 3, kRamp}});
        for (uint32_t ni = 0; ni < p->node_count; ++ni) {
            PlanNode& node = p->nodes[ni];
            if (node.kind != sonora::NodeKind::InsertChain) continue;
            node.first_plugin_lane = 0;
            node.plugin_lane_count = 1;
        }
        CHECK(rig.publish(p));
        rig.block();
        CHECK(rig.session->invalid_plan_blocks() == 1);
    }
    {
        Rig rig(2);
        std::vector<LaneSpec> many;
        for (uint32_t k = 0; k <= kMaxLanes; ++k) many.push_back({0, 0, k, {{0, 0.5f}}});
        CHECK(rig.publish(rig.plan({bound(0, 5), ChainBinding{}}, many)));
        rig.block();
        CHECK(rig.session->invalid_plan_blocks() == 1);
        CHECK(rig.runner.count == 0);
    }
    {
        // At the maximum, and a full ring of posted changes beside it: every
        // event has room.
        Rig rig(2);
        std::vector<LaneSpec> lanes;
        for (uint32_t k = 0; k < kMaxLanes; ++k) lanes.push_back({0, 0, k, {{0, 0.5f}}});
        CHECK(rig.publish(rig.plan({bound(0, 5), ChainBinding{}}, lanes)));
        rig.block();
        CHECK(rig.session->invalid_plan_blocks() == 0);
        CHECK(rig.runner.count == kMaxLanes);
        uint32_t posted = 0;
        while (rig.post(5, 9, posted, 0.0f)) ++posted;
        reset_heap_counts();
        rig.block();
        CHECK(heap_untouched());
        CHECK(rig.runner.count == kMaxLanes + posted + kMaxLanes);
    }
    return 0;
}

}  // namespace

int main() {
    if (test_follows_the_lane()) return 1;
    if (test_lane_wins()) return 1;
    if (test_modes()) return 1;
    if (test_new_plan()) return 1;
    if (test_bounds()) return 1;
    std::printf("plugin_automation: ok\n");
    return 0;
}
