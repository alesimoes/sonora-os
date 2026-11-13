// SPDX-License-Identifier: GPL-3.0-or-later
//
// Side-chain key edges (ADR-0058 items 2 and 3). A track keyed by an aux bus
// has an InsertChain node with a second input edge from that bus's AuxSum
// and key_channels equal to its channels; when the bus's sum is late (a
// track behind a 64-frame chain feeds it) compensate() delays the signal
// edge and not the key, and Session::process() hands the chain signal and
// key sample-aligned. A key bus that is gone keeps the chain keyed with a
// silent key, counted; an unbound chain is not keyed; a key loop an old
// file carries is refused by the builder, not crashed on.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "sonora/graph_levels.h"
#include "sonora/insert_chain_runner.h"
#include "sonora/key_edges.h"
#include "sonora/model/history.h"
#include "sonora/model/insert_key.h"
#include "sonora/model/operations.h"
#include "sonora/plan_builder.h"
#include "sonora/session.h"

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            return 1;                                                                  \
        }                                                                              \
    } while (0)

#define CHECK_APPLY(history, op)                                                                    \
    do {                                                                                            \
        std::string check_apply_error_;                                                             \
        if (!(history).apply((op), check_apply_error_)) {                                           \
            std::fprintf(stderr, "FAIL %s:%d: apply refused: %s\n", __FILE__, __LINE__,             \
                         check_apply_error_.c_str());                                               \
            return 1;                                                                               \
        }                                                                                           \
    } while (0)

namespace {

using namespace sonora::model;
using sonora::NodeKind;
using sonora::PlaybackPlan;
using sonora::PlanNode;

constexpr Id kVox = 100;
constexpr Id kKick = 102;
// Ids rise in the order keyed_session() applies them: the model refuses an
// id below its next fresh one.
constexpr Id kBus = 200;
constexpr Id kVoxInsert = 300;
constexpr Id kKickInsert = 301;
constexpr uint32_t kKickLatency = 64;

AddTrack track_op(Id track, Id playlist, const std::string& name) {
    AddTrack op;
    op.track = track;
    op.playlist = playlist;
    op.name = name;
    op.playlist_name = name + ".01";
    op.mix.input.push_back(0);
    return op;
}

AddInsert insert_op(Id track, Id insert) {
    AddInsert op;
    op.track = track;
    op.insert = insert;
    op.plugin = PluginRef{PluginFormat::Lv2, "urn:sonora:test:keyed", "Keyed", "1"};
    return op;
}

// Vox keyed by the bus; Kick, behind a chain, outputs to it.
bool keyed_session(History& h, std::string& error) {
    SetInsertKey key;
    key.track = kVox;
    key.insert = kVoxInsert;
    key.key_bus = kBus;
    const bool ok = h.apply(track_op(kVox, 101, "Vox"), error) && h.apply(track_op(kKick, 103, "Kick"), error) &&
                    h.apply(AddBus{kBus, "Key", BusMix{}}, error) && h.apply(insert_op(kVox, kVoxInsert), error) &&
                    h.apply(insert_op(kKick, kKickInsert), error) && h.apply(SetOutput{kKick, kBus}, error) &&
                    h.apply(key, error);
    if (!ok) std::fprintf(stderr, "keyed_session refused: %s\n", error.c_str());
    return ok;
}

sonora::BuildLimits limits() {
    sonora::BuildLimits l;
    l.engine_tracks = 2;
    l.max_segments = 64;
    l.max_chunk_slots = 1024;
    l.chunk_frames = 256;
    l.window_frames = 1024;
    l.pool_chunks = 128;
    l.max_delay_frames = 4096;
    l.delay_lines = 8;
    l.max_buses = 1;
    return l;
}

const sonora::SourceSlot kNoSources = [](const Source&, uint32_t&, std::string& e) {
    e = "no source expected";
    return false;
};

// Every chain bound; Kick's has the latency.
const sonora::ChainResolver kBindAll = [](const TrackEdit& t, uint32_t engine_track, sonora::ChainBinding& b) {
    b.channel_slot = engine_track;
    b.node_state = engine_track;
    b.param_store = engine_track;
    b.latency_frames = t.id == kKick ? kKickLatency : 0;
};

uint32_t find_node(const PlaybackPlan& p, NodeKind kind, uint64_t chain_id = 0) {
    for (uint32_t i = 0; i < p.node_count; ++i) {
        if (p.nodes[i].kind == kind && (chain_id == 0 || p.nodes[i].chain_id == chain_id)) return i;
    }
    return p.node_count;
}

bool forward(const PlaybackPlan& p) {
    for (uint32_t ni = 0; ni < p.node_count; ++ni) {
        const PlanNode& n = p.nodes[ni];
        for (uint32_t k = 0; k < n.input_edge_count; ++k) {
            const sonora::PlanEdge& e = p.edges[n.first_input_edge + k];
            if (e.to_node != ni || e.from_node >= ni) return false;
        }
    }
    return true;
}

int test_one_track_keyed_by_one_bus() {
    History h(new_session(SessionSettings{}));
    std::string error;
    CHECK(keyed_session(h, error));
    sonora::PlanContinuity continuity(8);
    PlaybackPlan* plan = nullptr;
    sonora::PlanReport report;
    CHECK(sonora::build_plan(h.current(), limits(), kNoSources, kBindAll, continuity, 1, plan, report, error));
    const PlaybackPlan& p = *plan;
    const uint32_t vox = find_node(p, NodeKind::InsertChain, kVox);
    const uint32_t sum = find_node(p, NodeKind::AuxSum);
    CHECK(vox < p.node_count && sum < p.node_count);
    const PlanNode& chain = p.nodes[vox];
    CHECK(chain.input_edge_count == 2);
    CHECK(chain.key_channels == chain.channels && chain.channels == 1);
    CHECK(p.nodes[p.edges[chain.first_input_edge].from_node].kind == NodeKind::TrackSource);
    CHECK(p.edges[chain.first_input_edge + 1].from_node == sum);
    // Kick's chain is not keyed.
    const PlanNode& kick = p.nodes[find_node(p, NodeKind::InsertChain, kKick)];
    CHECK(kick.key_channels == 0 && kick.input_edge_count == 1);
    CHECK(forward(p) && p.level_count > 0);
    CHECK(report.missing_keys == 0);

    // ADR-0012 §7.3 over the key: the bus's sum is the Kick chain's latency
    // late, so the signal waits that much and the key nothing.
    CHECK(p.edges[chain.first_input_edge].delay_frames == kKickLatency);
    CHECK(p.edges[chain.first_input_edge].delay_line != sonora::kNoDelayLine);
    CHECK(p.edges[chain.first_input_edge + 1].delay_frames == 0);
    CHECK(p.compensation_shortfall_frames == 0);
    sonora::destroy_plan(plan);
    return 0;
}

// An old file names a bus no longer there: the chain stays keyed, its key
// silent (no edge), and the builder counts it.
int test_missing_key_bus_is_a_silent_key() {
    History h(new_session(SessionSettings{}));
    std::string error;
    CHECK(keyed_session(h, error));
    Snapshot stale = h.current();
    auto edit = std::make_shared<EditState>(*stale.edit);
    auto vox = std::make_shared<TrackEdit>(*edit->tracks[0]);
    CHECK(vox->id == kVox && vox->inserts.size() == 1);
    vox->inserts[0].key_bus = 999;
    edit->tracks[0] = vox;
    stale.edit = edit;
    sonora::PlanContinuity continuity(8);
    PlaybackPlan* plan = nullptr;
    sonora::PlanReport report;
    CHECK(sonora::build_plan(stale, limits(), kNoSources, kBindAll, continuity, 1, plan, report, error));
    const PlanNode& chain = plan->nodes[find_node(*plan, NodeKind::InsertChain, kVox)];
    CHECK(chain.key_channels == chain.channels);
    CHECK(chain.input_edge_count == 1);
    CHECK(report.missing_keys == 1);
    CHECK(forward(*plan));
    sonora::destroy_plan(plan);
    return 0;
}

// No host bound: the chain passes through, so it has no key to hear.
int test_unbound_chain_is_not_keyed() {
    History h(new_session(SessionSettings{}));
    std::string error;
    CHECK(keyed_session(h, error));
    PlaybackPlan* plan = nullptr;
    sonora::PlanReport report;
    CHECK(sonora::build_plan(h.current(), limits(), kNoSources, 1, plan, report, error));
    const PlanNode& chain = plan->nodes[find_node(*plan, NodeKind::InsertChain, kVox)];
    CHECK(chain.key_channels == 0 && chain.input_edge_count == 1);
    CHECK(report.missing_keys == 0);
    sonora::destroy_plan(plan);
    return 0;
}

// The model refuses a key that closes a loop; an old file can still carry
// one (Vox keyed by the bus Vox outputs to). Refused, plan null.
int test_key_loop_is_refused() {
    History h(new_session(SessionSettings{}));
    std::string error;
    CHECK(keyed_session(h, error));
    Snapshot looped = h.current();
    auto edit = std::make_shared<EditState>(*looped.edit);
    auto vox = std::make_shared<TrackEdit>(*edit->tracks[0]);
    vox->output = kBus;
    edit->tracks[0] = vox;
    looped.edit = edit;
    sonora::PlanContinuity continuity(8);
    PlaybackPlan* plan = nullptr;
    sonora::PlanReport report;
    CHECK(!sonora::build_plan(looped, limits(), kNoSources, kBindAll, continuity, 1, plan, report, error));
    CHECK(plan == nullptr);
    CHECK(error.find("feedback loop") != std::string::npos);
    return 0;
}

// Chain 1 delays by its latency; chain 0, keyed, passes its signal through
// and checks each frame of it against its key.
class KeyProbe final : public sonora::InsertChainRunner {
public:
    explicit KeyProbe(uint32_t capacity) : ring_(capacity, 0.0f) {}
    void begin(uint64_t) noexcept SONORA_RT override {}
    void end() noexcept SONORA_RT override {}
    uint32_t block_frames() const noexcept SONORA_RT override { return 0; }
    void skip(const PlanNode&, float* const out[2], uint32_t frames) noexcept SONORA_RT override {
        for (uint32_t i = 0; i < frames; ++i) out[0][i] = out[1][i] = 0.0f;
    }
    void run(const PlanNode& node, const float* const in[2], float* const out[2], uint32_t frames,
             const sonora::ChainParamEvent*, uint32_t) noexcept SONORA_RT override {
        const auto cap = static_cast<uint32_t>(ring_.size());
        for (uint32_t i = 0; i < frames; ++i) {
            ring_[pos_] = in[0][i];
            out[0][i] = out[1][i] = ring_[(pos_ + cap - node.latency_frames) % cap];
            pos_ = (pos_ + 1) % cap;
        }
    }
    void run_keyed(const PlanNode& node, const float* const in[2], const float* const* key, float* const out[2],
                   uint32_t frames, const sonora::ChainParamEvent*, uint32_t) noexcept SONORA_RT override {
        ++keyed_runs;
        if (node.key_channels != 1 || key == nullptr || key[0] == nullptr) {
            ++unkeyed;
            return;
        }
        for (uint32_t i = 0; i < frames; ++i) {
            if (in[0][i] != key[0][i]) ++mismatches;
            if (key[0][i] != 0.0f) ++heard;
            out[0][i] = out[1][i] = in[0][i];
        }
    }
    uint64_t keyed_runs = 0, unkeyed = 0, mismatches = 0, heard = 0;

private:
    std::vector<float> ring_;
    uint32_t pos_ = 0;
};

float signal(int64_t t) { return 1.0f + static_cast<float>(t % 4096) / 4096.0f; }

bool publish(sonora::Session& session, PlaybackPlan* p) {
    sonora::DiskReader& r = session.reader();
    const uint64_t gen = r.serve(session.plans().current(), p);
    r.run_pass();
    if (r.resident() != gen) return false;
    if (!session.plans().publish(p)) return false;
    r.serve(p, nullptr);
    r.run_pass();
    return true;
}

// The plan as the builder lays it out, by hand over a Session's two input
// tracks: track 0 keyed by bus 0, track 1 behind a 64-frame chain into it.
int test_signal_and_key_are_sample_aligned() {
    sonora::SessionConfig cfg;
    cfg.dir = "test_key_edges_unused";
    cfg.track_count = 2;
    cfg.ring_seconds = 0.1;
    cfg.playback.chunk_frames = 256;
    cfg.playback.window_seconds = 0.02;
    cfg.playback.pool_chunks = 64;
    cfg.playback.max_block_frames = 256;
    cfg.playback.max_delay_frames = 4096;
    cfg.playback.max_buses = 1;
    sonora::Session session(cfg);
    for (auto& t : session.tracks()) t->set_monitor(sonora::Track::Monitor::Input);

    std::vector<sonora::ChainBinding> chains(2);
    for (uint32_t t = 0; t < 2; ++t) {
        chains[t].active = true;
        chains[t].channel_slot = t;
        chains[t].node_state = t;
        chains[t].chain_id = t + 1;
    }
    chains[1].latency_frames = kKickLatency;
    const uint32_t order[1] = {0};
    const uint32_t outputs[2] = {sonora::kNoBus, 0};
    sonora::MixLayout mix;
    mix.bus_count = 1;
    mix.bus_order = order;
    mix.track_outputs = outputs;
    const std::vector<uint32_t> keys = {0, sonora::kUnkeyed};
    const sonora::GraphSize g = sonora::mix_graph_size(2, chains.data(), mix);
    sonora::PlanSizes sizes;
    sizes.tracks = 2;
    sizes.nodes = g.nodes;
    sizes.edges = g.edges + sonora::key_edge_count(keys);
    PlaybackPlan* p = sonora::create_plan(sizes);
    p->epoch = session.plans().next_epoch();
    p->chunk_frames = 256;
    p->pan_exponent = 0.0f;   // a mono track at centre is 1 on each side
    for (uint32_t t = 0; t < 2; ++t) {
        p->tracks[t].engine_track = t;
        p->tracks[t].channels = 1;
    }
    sonora::layout_mix_graph(*p, chains.data(), mix);
    sonora::KeyEdgeResult keyed;
    std::string error;
    CHECK(sonora::add_key_edges(*p, keys, keyed, error));
    CHECK(keyed.edges == 1 && keyed.missing == 0);
    sonora::DelayLinePool pool(8);
    sonora::compensate(*p, session.max_delay_frames(), pool);
    CHECK(sonora::assign_plan_levels(*p));
    CHECK(publish(session, p));

    KeyProbe probe(4096);
    session.set_insert_chains(&probe);
    std::vector<float> in[2], out[2];
    const float* in_ptrs[2];
    float* out_ptrs[2];
    for (int c = 0; c < 2; ++c) {
        in[c].assign(256, 0.0f);
        out[c].assign(256, 0.0f);
        in_ptrs[c] = in[c].data();
        out_ptrs[c] = out[c].data();
    }
    int64_t frame = 0;
    for (int b = 0; b < 16; ++b) {
        const uint32_t n = 64;
        for (int c = 0; c < 2; ++c)
            for (uint32_t i = 0; i < n; ++i) in[c][i] = signal(frame + i);
        sonora::backend::AudioBlock block{};
        block.inputs = in_ptrs;
        block.input_count = 2;
        block.outputs = out_ptrs;
        block.output_count = 2;
        block.nframes = n;
        block.callback_ns = 1'000'000'000 + static_cast<uint64_t>(frame) * 1000;
        session.process(block);
        frame += n;
    }
    session.set_insert_chains(nullptr);
    CHECK(probe.keyed_runs == 16);
    CHECK(probe.unkeyed == 0);
    CHECK(probe.heard > 0);
    CHECK(probe.mismatches == 0);
    return 0;
}

}  // namespace

int main() {
    if (test_one_track_keyed_by_one_bus()) return 1;
    if (test_missing_key_bus_is_a_silent_key()) return 1;
    if (test_unbound_chain_is_not_keyed()) return 1;
    if (test_key_loop_is_refused()) return 1;
    if (test_signal_and_key_are_sample_aligned()) return 1;
    std::puts("key_edges: ok");
    return 0;
}
