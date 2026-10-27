// SPDX-License-Identifier: GPL-3.0-or-later
//
// Builds the immutable PlaybackPlan the audio thread plays from a model
// snapshot (ADR-0007 §2: engine/control builds, engine/core defines the
// layout; ADR-0009 §4; timeline-model.md, "The playback plan").
//
// Never on the audio thread: it allocates and reads the model.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/plan_graph.h"
#include "sonora/playback_plan.h"

namespace sonora {

// The session's fixed maxima (PlaybackConfig) and cache geometry. A plan
// that exceeds one is refused, naming it.
struct BuildLimits {
    uint32_t engine_tracks = 0;
    uint32_t channels_per_track = 1;
    uint32_t max_segments = 0;
    uint32_t max_chunk_slots = 0;
    uint32_t chunk_frames = 16384;
    int64_t window_frames = 0;
    uint32_t pool_chunks = 0;
    // The session's delay-line pool and per-line maximum (ADR-0012 §7.3).
    // Past either the plan is still built, and the shortfall reported.
    uint32_t max_delay_frames = 0;
    uint32_t delay_lines = 0;
    // The session's aux buses (PlaybackConfig::max_buses, ADR-0022 §8).
    uint32_t max_buses = 0;
};

// What the control thread knows of a track's chain when it builds: the host
// channel slot and its generation, the parameter store's index and the
// checked latency. The builder has set `active` (the track has an active
// insert) and `chain_id` (the track id) before the call. A latency counts
// only with a slot and a node state: without either the chain is laid out
// unbound and passes its input through with latency 0 (layout_track_graph).
// Until checklist 5's resolver nothing binds a slot; tests bind one here.
using ChainResolver = std::function<void(const model::TrackEdit& track, uint32_t engine_track,
                                         ChainBinding& binding)>;

// What one build hands the next, so a swap keeps each track's channel state:
// the delay line of each pair of stable ids, and the stable key of each
// model track. A track keeps its key for the session whatever its position,
// so a delete or a reorder never hands it another track's delay history.
// Copyable: the publisher builds on a copy of the playing plan's, so a plan
// that is cancelled before it plays leaves no trace.
struct PlanContinuity {
    explicit PlanContinuity(uint32_t delay_lines = 0) : pool(delay_lines) {}
    DelayLinePool pool;
    std::unordered_map<model::Id, uint32_t> track_keys;
    uint32_t next_key = 1;
    // The same for aux buses (ADR-0022 §5), from a counter of their own:
    // their nodes' ids carry tags no track node has.
    std::unordered_map<model::Id, uint32_t> bus_keys;
    uint32_t next_bus_key = 1;
};

// The engine source-table slot of a model source, registering it on first
// sight. False, with `error`, if its file cannot be opened.
using SourceSlot = std::function<bool(const model::Source& source, uint32_t& slot,
                                      std::string& error)>;

struct PlanReport {
    // Max over window positions of the chunks the plan needs resident
    // (ADR-0009 §4.3), and whether that is over half the pool: such a plan
    // may never become resident beside another, and is flagged before it
    // plays.
    uint64_t peak_chunk_demand = 0;
    bool over_half_pool = false;
    uint32_t segments = 0;
    uint32_t chunk_slots = 0;
    // PDC as built (ADR-0012 §7.3). A non-zero shortfall is shown in red:
    // the maximum or the delay-line pool left some path early by that much.
    uint32_t compensation_frames = 0;
    uint32_t compensation_shortfall_frames = 0;
    uint32_t clamped_edges = 0;
    uint32_t undelayed_edges = 0;
    // Per engine track: its chain's latency, and how early it reaches the
    // bus.
    std::vector<uint32_t> track_latency;
    std::vector<uint32_t> track_shortfall;
    // MIDI and instrument tracks, which this version plays nothing on
    // (ADR-0014 M-1): the engine says so rather than look broken.
    uint32_t silent_midi_tracks = 0;
};

// On success `out` is a plan with `epoch`, fully written, ready to hand to
// the disk reader as pending. On failure `out` is null and `error` says
// which limit or which object.
//
// Track i in display order plays on engine track i. Muted regions are left
// out. A crossfade extends its left region into its handle to cut + after
// and its right region back to cut - before, each faded over that span with
// the crossfade's shape, which replaces the region's own fade on that edge.
//
// A track with an active insert is TrackSource -> InsertChain -> TrackFader
// (ADR-0012 §7.1), bound through `chains` (may be empty); every edge into
// the bus is delayed so all tracks arrive aligned on the slowest, with lines
// and track keys from `continuity`, which the build updates.
//
// Aux bus i in display order is engine bus i, laid out after the tracks in
// an order where every bus comes after the buses that send to it, ties in
// display order; a track with a send is split at its pre-fader point
// (ADR-0022 §5). More buses than limits.max_buses, or sends that loop, are
// refused.
bool build_plan(const model::Snapshot& snapshot, const BuildLimits& limits,
                const SourceSlot& source_slot, const ChainResolver& chains,
                PlanContinuity& continuity, uint64_t epoch, PlaybackPlan*& out,
                PlanReport& report, std::string& error);
// No chain bound, a continuity of its own: for callers with no host and no
// plan before this one.
bool build_plan(const model::Snapshot& snapshot, const BuildLimits& limits,
                const SourceSlot& source_slot, uint64_t epoch, PlaybackPlan*& out,
                PlanReport& report, std::string& error);

// The plan's click segments (ADR-0020 §1): one per reachable entry of the
// session's tempo map, its beats the ticks on the grid of the bar in
// progress from the entry's tick to the next entry's, each at the frame
// model::MusicalMap::frame_at() gives it. Empty when the map does not
// build: the click is silent rather than wrong.
std::vector<ClickSegment> click_segments(const model::SessionSettings& settings);

// ((1 -+ pan) / 2) ^ exponent is `pan_law_db` at centre. The law is clamped
// to the model's range first, as gain_of_db() clamps.
float pan_exponent_for(float pan_law_db);

// 10 ^ (dB / 20), with dB clamped to [model::kMinGainDb, model::kMaxGainDb]
// and NaN taken as the quiet end. The model refuses anything outside, but a
// snapshot that never went through its validator must still put no inf, NaN
// or subnormal coefficient on the audio thread.
float gain_of_db(float db);

}  // namespace sonora
