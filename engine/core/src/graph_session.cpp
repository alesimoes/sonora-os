// SPDX-License-Identifier: GPL-3.0-or-later
//
// Session's side of the processing graph (ADR-0063): which nodes a worker may
// run, the walk by levels, and what a drop leaves for the node's readers.
//
// A node a worker was abandoned in may still be running blocks later (see
// graph_workers.h), and by then a new plan may give its index, its scratch
// buffer or its track to other nodes (the level pass reorders indices, so a
// buffer is not its node's index). So nothing here lets it do harm:
//  - before a level the audio thread arms each node it offers, with a copy
//    of the pass; a worker runs only once it moves the arm to claimed, and
//    runs from that copy. A claim that loses to the level's settling runs
//    nothing, so a late worker is one that won its claim;
//  - after the level, a dropped node whose arm is still claimed joins the
//    in-flight list with what it writes: its index, its scratch buffer and
//    its track's state (cursor, monitor ramp, fader target, meter). Until
//    its worker hands the arm back, nothing is armed at that index, and
//    every node of any plan that writes that buffer or that track's state
//    is dropped, worker node or audio-thread node (mix_or_fence()), so
//    nobody else writes them and their readers read the drop buffer. A sum
//    or an output that drew the buffer is silent for that while: rare (a
//    deadline miss and a plan swap together), and better than a race;
//  - it takes a PlanExchange hold before it looks for the plan, and runs
//    nothing unless a mix is open, so the plan it reads is one no reclaim
//    can free under it.
// It still reads upstream scratch and audio-thread state while the audio
// thread rewrites them: what it computes goes to a buffer nobody reads, and
// the cursor reseeks on its next run. That race is the accepted cost of
// never waiting (ADR-0063 §4).
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#include "sonora/graph_workers.h"
#include "sonora/session.h"

namespace sonora {

namespace {

enum : uint32_t { kArmIdle = 0, kArmReady = 1, kArmClaimed = 2 };
constexpr uint32_t kNoGraphTrack = ~uint32_t{0};

}  // namespace

class SessionGraphWork final : public GraphNodeWork {
public:
    explicit SessionGraphWork(Session& session) : session_(session) {}
    void run(uint32_t node) noexcept SONORA_RT override { session_.graph_run(node); }
    void drop(uint32_t node) noexcept SONORA_RT override { session_.graph_drop(node); }

private:
    Session& session_;
};

struct Session::GraphParts {
    explicit GraphParts(Session& session) : work(session) {}
    // Before `owned`, so it outlives the pool's join: an abandoned worker
    // may still be inside run() when the session goes.
    SessionGraphWork work;
    std::unique_ptr<GraphScheduler> owned;
};

void Session::destroy_graph_parts(GraphParts* parts) noexcept { delete parts; }

void Session::start_graph() {
    const PlaybackConfig& pb = cfg_.playback;
    auto parts = std::make_unique<GraphParts>(*this);
    if (cfg_.graph_scheduler != nullptr) {
        graph_sched_ = cfg_.graph_scheduler;
    } else if (pb.graph_workers == 0) {
        parts->owned = make_serial_graph_scheduler();
        graph_sched_ = parts->owned.get();
    } else {
        GraphSchedulerConfig config;
        config.workers = pb.graph_workers;
        config.max_nodes = scratch_buffers_;
        config.deadline_fraction = pb.graph_deadline_fraction;
        GraphWorkerOptions options;
        options.rt_priority = pb.graph_worker_priority;
        GraphWorkersStarted started = start_graph_workers(config, options);
        if (!started.pool) throw std::runtime_error("graph workers: " + started.error);
        graph_pool_ = started.pool.get();
        parts->owned = std::move(started.pool);
        graph_sched_ = parts->owned.get();
    }
    graph_work_ = &parts->work;
    graph_.reset(parts.release());
    graph_parallel_ = cfg_.graph_scheduler != nullptr || graph_sched_->workers() > 0;
    if (!graph_parallel_) return;
    // Value-initialised: every page is touched here, not on the audio thread.
    const std::size_t nodes = static_cast<std::size_t>(scratch_buffers_) + 1;
    graph_list_.reset(new uint32_t[nodes]());
    graph_dropped_.reset(new uint64_t[nodes]());
    graph_drop_buf_.reset(new float[nodes * 2 * max_block_]());
    graph_last_.reset(new float[2 * nodes]());
    graph_arm_.reset(new std::atomic<uint32_t>[nodes]);
    for (std::size_t i = 0; i < nodes; ++i) graph_arm_[i].store(kArmIdle, std::memory_order_relaxed);
    graph_pass_.reset(new MixPass[nodes]());
    graph_flight_.reset(new GraphFlight[nodes]());
    graph_fence_node_.reset(new uint8_t[nodes]());
    graph_fence_buf_.reset(new uint8_t[nodes]());
    graph_fence_track_.reset(new uint8_t[tracks_.size() + 1]());
}

bool Session::graph_parallel_safe(const PlanNode& node) const noexcept SONORA_RT {
    switch (node.kind) {
        // vs_buf_ is one buffer for every track.
        case NodeKind::TrackSource: return !vs_on_;
        case NodeKind::TrackFader:
        case NodeKind::AuxFader: return true;
        // §7: the chain protocol, sums, delay lines and the device stay on
        // the audio thread; TrackOut renders through the shared track_buf_.
        default: return false;
    }
}

void Session::adopt_graph_plan(const PlaybackPlan& plan) noexcept SONORA_RT {
    graph_sched_->reset_drops();
    graph_levels_ok_ = false;
    if (!graph_parallel_) return;
    // The in-flight list stays: it names what a late worker writes, and the
    // new plan's nodes are fenced by the same names.
    std::memset(graph_last_.get(), 0, 2 * (static_cast<std::size_t>(scratch_buffers_) + 1) * sizeof(float));
    const uint32_t levels = plan.level_count;
    if (!plan_valid_ || levels == 0 || plan.level_first == nullptr || levels > plan.node_count ||
        plan.level_first[0] != 0 || plan.level_first[levels] != plan.node_count) {
        return;
    }
    for (uint32_t l = 0; l < levels; ++l) {
        if (plan.level_first[l] > plan.level_first[l + 1]) return;
    }
    // A node a worker may take reads only lower levels; a plan that says
    // otherwise is walked serially, as one with no levels.
    for (uint32_t ni = 0; ni < plan.node_count; ++ni) {
        const PlanNode& node = plan.nodes[ni];
        if (node.kind != NodeKind::TrackSource && node.kind != NodeKind::TrackFader &&
            node.kind != NodeKind::AuxFader) {
            continue;
        }
        if (node.level >= levels || ni < plan.level_first[node.level] ||
            ni >= plan.level_first[node.level + 1]) {
            return;
        }
        for (uint32_t k = 0; k < node.input_edge_count; ++k) {
            if (plan.edges[node.first_input_edge + k].from_node >= plan.level_first[node.level]) return;
        }
    }
    graph_levels_ok_ = true;
}

void Session::mix_graph() noexcept SONORA_RT {
    const PlaybackPlan& plan = *pass_.plan;
    if (graph_parallel_) refresh_graph_fences();
    if (!graph_levels_ok_) {
        for (uint32_t ni = 0; ni < plan.node_count; ++ni) mix_or_fence(ni);
        return;
    }
    const uint64_t period_ns =
        cfg_.sample_rate != 0 ? uint64_t{block_frames_} * 1'000'000'000ull / cfg_.sample_rate : 0;
    const uint64_t deadline =
        graph_deadline_ns(last_callback_ns_, period_ns, cfg_.playback.graph_deadline_fraction);
    const uint32_t n = pass_.n;
    graph_open_.store(true, std::memory_order_seq_cst);
    for (uint32_t l = 0; l < plan.level_count; ++l) {
        uint32_t first = 0;
        uint32_t count = 0;
        plan_level_range(plan, l, first, count);
        uint32_t offered = 0;
        for (uint32_t ni = first; ni < first + count; ++ni) {
            if (!graph_parallel_safe(plan.nodes[ni])) continue;
            // An index still claimed cannot be armed again: its arm and its
            // pass copy belong to the late worker.
            if (graph_flights_ != 0 && (graph_fence_node_[ni] != 0 || graph_fenced_node(plan, ni))) {
                fence_drop(ni);
                continue;
            }
            graph_arm(ni);
            graph_list_[offered++] = ni;
        }
        if (offered != 0) {
            graph_sched_->run_level(GraphLevel{graph_list_.get(), offered, deadline}, *graph_work_);
            for (uint32_t k = 0; k < offered; ++k) graph_settle(graph_list_[k]);
            // Where a later drop's ramp starts: read here, on the audio
            // thread, once the scheduler has handed the scratch back.
            for (uint32_t k = 0; n != 0 && k < offered; ++k) {
                const uint32_t ni = graph_list_[k];
                if (graph_dropped_[ni] == mix_stamp_) continue;
                const uint32_t buffer = plan.nodes[ni].scratch_buffer;
                graph_last_[2 * ni] = scratch(buffer, 0)[n - 1];
                graph_last_[2 * ni + 1] = scratch(buffer, 1)[n - 1];
            }
        }
        // The rest of the level in plan order: nodes of one level are
        // independent, and the chain protocol keeps its order across levels.
        for (uint32_t ni = first; ni < first + count; ++ni) {
            if (!graph_parallel_safe(plan.nodes[ni])) mix_or_fence(ni);
        }
    }
    // seq_cst, before the next callback's PlanExchange::acquire(): a worker
    // that sees it false after its hold runs nothing (graph_run()).
    graph_open_.store(false, std::memory_order_seq_cst);
}

void Session::graph_run(uint32_t ni) noexcept SONORA_RT {
    plans_->hold();
    MixPass pass;
    if (graph_open_.load(std::memory_order_seq_cst) && graph_claim(ni, pass)) {
        mix_node(pass, ni);
        graph_return(ni);
    }
    plans_->release();
}

void Session::graph_arm(uint32_t ni) noexcept SONORA_RT {
    // Not in flight (it would be fenced), so no worker reads the copy now.
    graph_pass_[ni] = pass_;
    // Release: a claimer that sees it armed sees the copy.
    graph_arm_[ni].store(kArmReady, std::memory_order_release);
}

bool Session::graph_claim(uint32_t ni, MixPass& pass) noexcept SONORA_RT {
    uint32_t expect = kArmReady;
    if (!graph_arm_[ni].compare_exchange_strong(expect, kArmClaimed, std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        return false;
    }
    // The audio thread rewrites it only after graph_return().
    pass = graph_pass_[ni];
    return true;
}

void Session::graph_return(uint32_t ni) noexcept SONORA_RT {
    // Release: whoever sees it idle sees every write of the run.
    graph_arm_[ni].store(kArmIdle, std::memory_order_release);
}

void Session::graph_settle(uint32_t ni) noexcept SONORA_RT {
    uint32_t expect = kArmReady;
    // Never claimed, and from here no claim can start it.
    if (graph_arm_[ni].compare_exchange_strong(expect, kArmIdle, std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
        return;
    }
    if (expect != kArmClaimed) return;  // ran and returned
    // Claimed and not back: a node the scheduler dropped at the deadline.
    const PlaybackPlan& plan = *pass_.plan;
    const uint32_t entry = graph_flights_++;
    graph_flight_[entry] =
        GraphFlight{ni, plan.nodes[ni].scratch_buffer, graph_state_track(plan, ni)};
    // Now, not at the next mix: a later level of this one may hold its track.
    set_graph_fence(entry, 1);
}

void Session::refresh_graph_fences() noexcept SONORA_RT {
    if (graph_flights_ == 0) return;
    // Cleared first, then set again for those still out: two entries may
    // share a track.
    for (uint32_t k = 0; k < graph_flights_; ++k) set_graph_fence(k, 0);
    uint32_t kept = 0;
    for (uint32_t k = 0; k < graph_flights_; ++k) {
        // Acquire: a returned worker's writes are seen before anyone reuses
        // what it wrote.
        if (graph_arm_[graph_flight_[k].node].load(std::memory_order_acquire) == kArmIdle) continue;
        graph_flight_[kept++] = graph_flight_[k];
    }
    graph_flights_ = kept;
    for (uint32_t k = 0; k < kept; ++k) set_graph_fence(k, 1);
}

void Session::set_graph_fence(uint32_t entry, uint8_t on) noexcept SONORA_RT {
    const GraphFlight& f = graph_flight_[entry];
    graph_fence_node_[f.node] = on;
    graph_fence_buf_[f.buffer] = on;
    if (f.track != kNoGraphTrack) graph_fence_track_[f.track] = on;
}

uint32_t Session::graph_state_track(const PlaybackPlan& plan, uint32_t ni) const noexcept SONORA_RT {
    const PlanNode& node = plan.nodes[ni];
    if (node.kind != NodeKind::TrackSource && node.kind != NodeKind::TrackFader &&
        node.kind != NodeKind::TrackOut) {
        return kNoGraphTrack;
    }
    if (node.track >= plan.track_count) return kNoGraphTrack;
    const uint32_t track = plan.tracks[node.track].engine_track;
    return track < tracks_.size() ? track : kNoGraphTrack;
}

bool Session::graph_fenced_node(const PlaybackPlan& plan, uint32_t ni) const noexcept SONORA_RT {
    if (graph_fence_buf_[plan.nodes[ni].scratch_buffer] != 0) return true;
    const uint32_t track = graph_state_track(plan, ni);
    return track != kNoGraphTrack && graph_fence_track_[track] != 0;
}

void Session::mix_or_fence(uint32_t ni) noexcept SONORA_RT {
    if (graph_flights_ == 0 || !graph_fenced_node(*pass_.plan, ni)) {
        mix_node(pass_, ni);
        return;
    }
    fence_drop(ni);
}

void Session::fence_drop(uint32_t ni) noexcept SONORA_RT {
    graph_drop(ni);
    graph_fenced_.fetch_add(1, std::memory_order_relaxed);
}

void Session::graph_drop(uint32_t ni) noexcept SONORA_RT {
    if (graph_dropped_ == nullptr) return;
    graph_dropped_[ni] = mix_stamp_;
    const uint32_t n = pass_.n;
    float* const L = graph_drop_buf_.get() + static_cast<std::size_t>(ni) * 2 * max_block_;
    float* const R = L + max_block_;
    const float l0 = graph_last_[2 * ni];
    const float r0 = graph_last_[2 * ni + 1];
    // A ramp from the last frame the node played, so the dip does not click
    // (ADR-0063 §4, as a missed chain, ADR-0012 §3); silence after it.
    const float inv = n != 0 ? 1.0f / static_cast<float>(n) : 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        const float a = 1.0f - static_cast<float>(i + 1) * inv;
        L[i] = l0 * a;
        R[i] = r0 * a;
    }
    graph_last_[2 * ni] = 0.0f;
    graph_last_[2 * ni + 1] = 0.0f;
}

uint32_t Session::graph_workers() const { return graph_sched_->workers(); }

GraphDropCounts Session::graph_drops() const { return graph_sched_->drop_counts(); }

uint64_t Session::graph_late_results() const {
    return graph_pool_ != nullptr ? graph_pool_->late_results() : 0;
}

}  // namespace sonora
