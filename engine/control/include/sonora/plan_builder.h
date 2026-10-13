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

#include "sonora/model/model.h"
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
};

// On success `out` is a plan with `epoch`, fully written, ready to hand to
// the disk reader as pending. On failure `out` is null and `error` says
// which limit or which object.
//
// Track i in display order plays on engine track i. Muted regions are left
// out. A crossfade extends its left region into its handle to cut + after
// and its right region back to cut - before, each faded over that span with
// the crossfade's shape, which replaces the region's own fade on that edge.
bool build_plan(const model::Snapshot& snapshot, const BuildLimits& limits,
                const SourceSlot& source_slot, uint64_t epoch, PlaybackPlan*& out,
                PlanReport& report, std::string& error);

// ((1 -+ pan) / 2) ^ exponent is `pan_law_db` at centre. The law is clamped
// to the model's range first, as gain_of_db() clamps.
float pan_exponent_for(float pan_law_db);

// 10 ^ (dB / 20), with dB clamped to [model::kMinGainDb, model::kMaxGainDb]
// and NaN taken as the quiet end. The model refuses anything outside, but a
// snapshot that never went through its validator must still put no inf, NaN
// or subnormal coefficient on the audio thread.
float gain_of_db(float db);

}  // namespace sonora
