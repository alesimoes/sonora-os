// SPDX-License-Identifier: GPL-3.0-or-later
#include "bench_session.h"

#include <chrono>
#include <cmath>

#include "heap_counter.h"
#include "sonora/rt.h"

namespace sonora::bench {

namespace {

// The ring each fake chain delays through: past the aligned chain's latency
// plus a callback, a power of two so the index is a mask.
constexpr uint32_t kRingFrames = 8192;

uint64_t steady_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

SessionConfig session_config(const BenchConfig& b) {
    SessionConfig cfg;
    cfg.dir = "sonora_graph_bench_unused";
    cfg.sample_rate = b.rate;
    cfg.track_count = int(b.tracks());
    cfg.channels_per_track = 1;
    cfg.ring_seconds = 0.1;
    cfg.playback.chunk_frames = 256;
    cfg.playback.window_seconds = 0.02;
    cfg.playback.pool_chunks = 64;
    cfg.playback.max_block_frames = b.quantum < 256 ? 256 : b.quantum;
    cfg.playback.max_delay_frames = 16384;
    // Every track but the aligned one is delayed to meet it.
    cfg.playback.max_delayed_edges = 2 * b.tracks() + 8;
    return cfg;
}

}  // namespace

float bench_signal(int64_t t, uint32_t lead_in, float amplitude) {
    if (t < int64_t(lead_in)) return 0.f;
    uint32_t x = uint32_t(t) * 2654435761u + 0x9E3779B9u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return amplitude * (float(int32_t(x >> 8) % 4096) / 4096.f);
}

// ---- the fake runner ----

class FakeChainSource::Runner final : public InsertChainRunner {
public:
    Runner(uint32_t slots, uint32_t stages, uint32_t quantum, uint64_t budget_ns, int64_t overrun_at,
           FakeClock* clock)
        : stages_(stages), quantum_(quantum), budget_ns_(budget_ns), overrun_at_(overrun_at), clock_(clock),
          ring_(size_t(slots) * kRingFrames, 0.f), pos_(slots, 0), state_(size_t(slots) * stages * 4, 0.f) {}

    void begin(uint64_t) noexcept SONORA_RT override {
        if (open_) ++sequence_errors_;
        open_ = true;
        stall_ = (callback_ == overrun_at_);
    }
    void end() noexcept SONORA_RT override {
        if (!open_) ++sequence_errors_;
        open_ = false;
        ++callback_;
    }
    uint32_t block_frames() const noexcept SONORA_RT override { return quantum_; }
    void skip(const PlanNode&, float* const out[2], uint32_t frames) noexcept SONORA_RT override {
        for (uint32_t i = 0; i < frames; ++i) out[0][i] = out[1][i] = 0.f;
        ++skips_;
    }
    void run(const PlanNode& node, const float* const in[2], float* const out[2], uint32_t frames,
             const ChainParamEvent*, uint32_t) noexcept SONORA_RT override {
        if (!open_ || node.channel_slot >= pos_.size()) {
            ++sequence_errors_;
            for (uint32_t i = 0; i < frames; ++i) out[0][i] = out[1][i] = 0.f;
            return;
        }
        float* ring = ring_.data() + size_t(node.channel_slot) * kRingFrames;
        float* st = state_.data() + size_t(node.channel_slot) * stages_ * 4;
        uint32_t pos = pos_[node.channel_slot];
        const uint32_t d = node.latency_frames;
        for (uint32_t i = 0; i < frames; ++i) {
            float x = in[0][i];
            // Direct form I with b0 = 1 and every other coefficient 0: the
            // products are exact zeros, so the stage is the identity, bit
            // for bit, at a biquad's cost.
            for (uint32_t s = 0; s < stages_; ++s) {
                float* z = st + s * 4;
                const float y = 1.f * x + 0.f * z[0] + 0.f * z[1] - 0.f * z[2] - 0.f * z[3];
                z[1] = z[0];
                z[0] = x;
                z[3] = z[2];
                z[2] = y;
                x = y;
            }
            ring[pos] = x;
            out[0][i] = ring[(pos + kRingFrames - d) & (kRingFrames - 1)];
            out[1][i] = out[0][i];
            pos = (pos + 1) & (kRingFrames - 1);
        }
        pos_[node.channel_slot] = pos;
        if (stall_) {
            // The injected over-budget plugin: it holds this callback past
            // twice the budget, so the driver's timing must see a miss.
            stall_ = false;
            if (clock_) {
                clock_->ns += 2 * budget_ns_;
            } else {
                const uint64_t until = steady_ns() + 2 * budget_ns_;
                while (steady_ns() < until) {
                }
            }
        }
    }

    uint64_t sequence_errors() const { return sequence_errors_; }
    uint64_t skips() const { return skips_; }

private:
    uint32_t stages_, quantum_;
    uint64_t budget_ns_;
    int64_t overrun_at_;
    FakeClock* clock_;
    int64_t callback_ = 0;
    bool open_ = false, stall_ = false;
    uint64_t sequence_errors_ = 0, skips_ = 0;
    std::vector<float> ring_;
    std::vector<uint32_t> pos_;
    std::vector<float> state_;
};

FakeChainSource::FakeChainSource(const BenchConfig& cfg)
    : cfg_(cfg),
      runner_(std::make_unique<Runner>(cfg.tracks(), cfg.plugins_per_chain, cfg.quantum, cfg.budget_ns(),
                                       cfg.inject_overrun_at, cfg.fake_clock)) {}

FakeChainSource::~FakeChainSource() = default;

InsertChainRunner& FakeChainSource::runner() { return *runner_; }

uint32_t FakeChainSource::latency(uint32_t track) const {
    // The load chains report one block, as a host's IPC block does.
    return track == cfg_.aligned_track() ? cfg_.aligned_latency : cfg_.quantum;
}

ChainBinding FakeChainSource::binding(uint32_t track) const {
    ChainBinding b;
    if (track == cfg_.dry_track()) return b;
    b.stable_key = track + 1;
    b.active = true;
    b.chain_id = 1000 + track;
    b.channel_slot = track;
    b.param_store = track;
    b.node_state = track;
    b.latency_frames = latency(track);
    return b;
}

uint64_t FakeChainSource::runner_drops() const { return runner_->sequence_errors() + runner_->skips(); }

// ---- the rig ----

BenchRig::BenchRig(const BenchConfig& cfg, ChainSource& chains)
    : cfg_(cfg), chains_(chains), session_(std::make_unique<Session>(session_config(cfg))),
      pool_(session_->delay_lines()), lead_in_(4 * cfg.quantum) {
    const uint32_t tracks = cfg.tracks();
    in_.assign(tracks, std::vector<float>(cfg.quantum, 0.f));
    for (auto& v : in_) in_ptrs_.push_back(v.data());
    for (int o = 0; o < 2; ++o) {
        out_[o].assign(cfg.quantum, 0.f);
        out_ptrs_[o] = out_[o].data();
    }
    left_.assign(size_t(cfg.capture_blocks) * cfg.quantum, 0.f);
    right_.assign(left_.size(), 0.f);
    for (uint32_t t = 0; t < tracks; ++t) {
        Track& tr = *session_->tracks()[t];
        tr.set_monitor(Track::Monitor::Input);
        tr.set_pan(t == cfg.dry_track() ? 1.f : -1.f);
        if (t < cfg.load_tracks) tr.set_gain(0.f);
    }
    session_->set_insert_chains(&chains_.runner());
    ready_ = publish();
    block_.inputs = in_ptrs_.data();
    block_.input_count = uint32_t(in_ptrs_.size());
    block_.outputs = out_ptrs_;
    block_.output_count = 2;
    block_.nframes = cfg.quantum;
    // The faders' start-up ramp and the hosts' ramp-in sit in the lead-in;
    // the window starts a block after the signal reaches the bus.
    const uint64_t at_bus = (uint64_t(lead_in_) + comp_.compensation_frames) / cfg.quantum + 2;
    capture_from_ = at_bus;
}

BenchRig::~BenchRig() { session_.reset(); }

bool BenchRig::publish() {
    const uint32_t tracks = cfg_.tracks();
    std::vector<ChainBinding> b(tracks);
    uint32_t active = 0;
    for (uint32_t t = 0; t < tracks; ++t) {
        b[t] = chains_.binding(t);
        active += b[t].active ? 1 : 0;
    }
    PlanSizes sizes;
    sizes.tracks = tracks;
    sizes.nodes = graph_nodes(tracks, active);
    sizes.edges = graph_edges(tracks, active);
    PlaybackPlan* p = create_plan(sizes);
    p->epoch = session_->plans().next_epoch();
    p->chunk_frames = 256;
    p->pan_exponent = 0.5f;
    for (uint32_t t = 0; t < tracks; ++t) {
        p->tracks[t].engine_track = t;
        p->tracks[t].channels = 1;
    }
    layout_track_graph(*p, b.data());
    comp_ = compensate(*p, session_->max_delay_frames(), pool_, nullptr);
    // As the control thread publishes: the disk reader has the plan
    // resident before the session may adopt it.
    DiskReader& r = session_->reader();
    const uint64_t gen = r.serve(session_->plans().current(), p);
    r.run_pass();
    if (r.resident() != gen) return false;
    if (!session_->plans().publish(p)) return false;
    r.serve(p, nullptr);
    r.run_pass();
    return comp_.shortfall_frames == 0 && comp_.clamped_edges == 0 && comp_.undelayed_edges == 0;
}

void BenchRig::prepare(uint64_t callback_ns) {
    for (auto& v : in_)
        for (uint32_t i = 0; i < cfg_.quantum; ++i) v[i] = bench_signal(frame_ + i, lead_in_, 0.1f);
    // What a backend stamps: a host paces on it (ADR-0012 §1.3).
    block_.callback_ns = callback_ns;
}

void BenchRig::process() {
    t_counting = true;
    session_->process(block_);
    t_counting = false;
    frame_ += cfg_.quantum;
}

void BenchRig::capture(uint64_t index) {
    if (index < capture_from_ || index >= capture_from_ + cfg_.capture_blocks) return;
    const size_t at = size_t(index - capture_from_) * cfg_.quantum;
    for (uint32_t i = 0; i < cfg_.quantum; ++i) {
        left_[at + i] = out_[0][i];
        right_[at + i] = out_[1][i];
    }
}

BenchRig::Alignment BenchRig::alignment() const {
    Alignment a;
    const int64_t n = int64_t(left_.size());
    const int64_t span = 2 * int64_t(cfg_.quantum);
    if (n <= 2 * span) return a;
    uint64_t nonzero = 0;
    for (int64_t i = span; i < n - span; ++i) {
        a.error = std::fmax(a.error, std::fabs(double(left_[size_t(i)]) - double(right_[size_t(i)])));
        nonzero += right_[size_t(i)] != 0.f ? 1 : 0;
    }
    // A window of silence would align at any lag: it is not a comparison.
    a.compared = nonzero > uint64_t(n - 2 * span) / 2;
    double best = 1e300;
    for (int64_t lag = -span; lag <= span; ++lag) {
        double e = 0.0;
        for (int64_t i = span; i < n - span; ++i) e += std::fabs(double(left_[size_t(i)]) - double(right_[size_t(i - lag)]));
        if (e < best) {
            best = e;
            a.lag = lag;
        }
    }
    return a;
}

uint64_t BenchRig::drops() const {
    const GraphDropCounts g = session_->graph_drops();
    return g.dropped + g.skipped + session_->graph_fenced() + session_->unrun_chain_blocks() +
           session_->chain_bypassed_blocks() + session_->silent_chain_blocks() + session_->invalid_plan_blocks() +
           chains_.runner_drops();
}

}  // namespace sonora::bench
