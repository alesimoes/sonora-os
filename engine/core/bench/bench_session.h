// SPDX-License-Identifier: GPL-3.0-or-later
//
// The session sonora-graph-bench drives (ADR-0173 §1), built as
// engine/core/tests/test_session_plugins.cpp builds it: every track
// monitoring its input, the chains bound, the plan laid out by
// layout_track_graph and compensated, then published through the disk
// reader. Tracks, in order:
//
//   0 .. load-1   the load: a 4-plugin chain each, fader at 0 (the chain is
//                 before the fader, so it still runs on the signal) so the
//                 bus carries only the alignment pair
//   load          the 2048-frame chain, hard left
//   load + 1      a dry track, hard right
//
// The driver (graph_bench.cpp) calls block() once per callback and times
// it; capture() copies the bus out after the timed region, into arrays
// sized here.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "sonora/insert_chain_runner.h"
#include "sonora/plan_graph.h"
#include "sonora/session.h"

namespace sonora::bench {

// The clock a --fake run reads: it moves only when the injected plugin
// stalls, so a miss is a fact of the fixture, never of the machine.
struct FakeClock {
    uint64_t ns = 0;
};

struct BenchConfig {
    uint32_t load_tracks = 18;
    uint32_t plugins_per_chain = 4;
    uint32_t quantum = 128;
    uint32_t rate = 48000;
    uint32_t callbacks = 20000;
    // The aligned chain's own latency; the fake runner adds no IPC block.
    uint32_t aligned_latency = 2048;
    // Callback index the fake runner stalls in, over budget (-1: none).
    int64_t inject_overrun_at = -1;
    uint32_t capture_blocks = 64;
    // Set for --fake (not owned); null reads the steady clock.
    FakeClock* fake_clock = nullptr;

    uint32_t tracks() const { return load_tracks + 2; }
    uint32_t aligned_track() const { return load_tracks; }
    uint32_t dry_track() const { return load_tracks + 1; }
    uint64_t budget_ns() const { return uint64_t(quantum) * 1000000000ull / rate; }
};

// Deterministic noise, 0 before the lead-in, |x| <= amplitude: a one-frame
// misalignment is an error of the signal's own size.
float bench_signal(int64_t t, uint32_t lead_in, float amplitude);

// What the session's chains run through, and the per-run counters the
// verdict reads from it.
class ChainSource {
public:
    virtual ~ChainSource() = default;
    virtual InsertChainRunner& runner() = 0;
    // The binding of track t's chain (inactive for the dry track).
    virtual ChainBinding binding(uint32_t track) const = 0;
    // The latency a chain of `track` reports, frames.
    virtual uint32_t latency(uint32_t track) const = 0;
    // Chain blocks returned late after the first callback, and blocks the
    // runner gave up on (removed hosts); 0 for the fake.
    virtual uint64_t host_misses(uint64_t callbacks) const = 0;
    virtual uint64_t runner_drops() const = 0;
};

// The fake: per node, four stages of an identity biquad (b0 = 1: exact, so
// the alignment check stays bit for bit, yet each stage does a biquad's
// arithmetic) and a ring delaying by the node's latency, all sized here.
class FakeChainSource final : public ChainSource {
public:
    explicit FakeChainSource(const BenchConfig& cfg);
    ~FakeChainSource() override;
    InsertChainRunner& runner() override;
    ChainBinding binding(uint32_t track) const override;
    uint32_t latency(uint32_t track) const override;
    uint64_t host_misses(uint64_t) const override { return 0; }
    uint64_t runner_drops() const override;

private:
    class Runner;
    BenchConfig cfg_;
    std::unique_ptr<Runner> runner_;
};

class BenchRig {
public:
    BenchRig(const BenchConfig& cfg, ChainSource& chains);
    ~BenchRig();
    bool ready() const { return ready_; }
    const Compensation& compensation() const { return comp_; }
    Session& session() { return *session_; }

    // Fills the inputs for this callback; untimed.
    void prepare(uint64_t callback_ns);
    // Session::process() once, under the heap counter. The driver times it.
    void process();
    // After the timed region: keeps the bus if this callback is in the
    // alignment window.
    void capture(uint64_t index);
    // First callback whose output is captured.
    uint64_t capture_from() const { return capture_from_; }

    // The best lag in +-2 blocks of left (the aligned chain) against right
    // (the dry track) over the window, and the worst error at lag 0.
    // compared: the window was filled.
    struct Alignment {
        bool compared = false;
        int64_t lag = 0;
        double error = 0.0;
    };
    Alignment alignment() const;
    // Node blocks the session dropped or played silent (ADR-0063 drops,
    // fenced, unrun, bypassed, silent, invalid plan) plus the source's.
    uint64_t drops() const;

private:
    bool publish();

    BenchConfig cfg_;
    ChainSource& chains_;
    std::unique_ptr<Session> session_;
    DelayLinePool pool_;
    Compensation comp_;
    std::vector<std::vector<float>> in_;
    std::vector<const float*> in_ptrs_;
    std::vector<float> out_[2];
    float* out_ptrs_[2] = {nullptr, nullptr};
    std::vector<float> left_, right_;
    backend::AudioBlock block_{};
    int64_t frame_ = 0;
    uint32_t lead_in_ = 0;
    uint64_t capture_from_ = 0;
    bool ready_ = false;
};

}  // namespace sonora::bench
