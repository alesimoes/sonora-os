// SPDX-License-Identifier: GPL-3.0-or-later
//
// audio.elastic's checks and edit (ADR-0136 §2, §5), on the control thread:
// what the request resolves to in the document before anything is read, and
// the one Compound that points the region at the stretched copy once it is
// written. Pure over the model; the files are elastic_job.h's.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sonora/api/elastic.h"
#include "sonora/model/model.h"
#include "sonora/model/musical_map.h"
#include "sonora/model/operations.h"
#include "sonora/stretch/stretcher.h"
#include "sonora/stretch/warp_map.h"

namespace sonora::elastic {

// Source frames kept either side of the region in the copy, at their own
// speed, so a later trim or crossfade has material (as a sample edit's).
inline constexpr double kElasticHandleSeconds = 2.0;

// One region channel's audio: a file of the session and its channel in it.
struct ChannelFile {
    std::string path;   // relative to the session folder
    std::uint16_t channel = 0;
};

struct ElasticPlan {
    api::ElasticRequest request;
    model::Region region;          // as it was when the job started
    model::Id playlist = 0;
    model::Id track = 0;
    std::string track_name;
    std::vector<ChannelFile> files;   // one per region channel
    std::uint32_t sample_rate = 0;
    model::AudioFormat format = model::AudioFormat::Int24;
    // The copy covers source frames [s0, s0 + lead + region.length + tail).
    model::Frames s0 = 0;
    model::Frames lead = 0;
    model::Frames tail = 0;
    std::int64_t time_reference = 0;   // of the copy's first frame
    // The longest the region may become: up to the next region.
    model::Frames max_length = api::kMaxElasticFrames;
    // Region-relative markers (ratio, warp); quantize's come from the onsets.
    std::vector<stretch::Marker> markers;
    model::MusicalMap tempo;   // quantize only
};

// What the worker wrote, as the document will name it.
struct ElasticOutput {
    model::Source source;   // path and numbers read back from the file; no id yet
    model::Frames region_length = 0;
    std::vector<stretch::Marker> markers;   // region-relative, as stretched
};

// Resolves `request` against `now`: an audio region whose files are in the
// pool, a request valid for its length, markers that fit, and, for ratio and
// warp, a result that ends before the next region. False, with the reason
// the operator is shown; nothing is read or written then.
bool elastic_check(const model::Snapshot& now, const api::ElasticRequest& request,
                   ElasticPlan& out, std::string& why);

// The longer side of the overlap rule, for quantize once its length is
// known: false, saying by how much, if `length` passes plan.max_length.
bool fits_before_next(const ElasticPlan& plan, model::Frames length, std::string& why);

// The map over the whole input (handles included) for region-relative
// `markers`: the handles keep their speed. False, with the reason, for
// markers the map refuses.
bool input_map(const ElasticPlan& plan, const std::vector<stretch::Marker>& markers,
               stretch::WarpMap& out, std::string& why);

// Where input frame `src` lands in the output of `map` (the inverse of
// WarpMap::source_at), for the gain line.
double target_at(const stretch::WarpMap& map, double src);

// The Compound against `now`: AddSource, then SetRegionSources and a trim of
// the region's end in the order the model accepts (shrinking trims first).
// False, with the reason, if the region changed or moved meanwhile, or the
// result would now reach the next region.
bool elastic_compound(const model::Snapshot& now, const ElasticPlan& plan,
                      const ElasticOutput& output, model::Compound& out, std::string& why);

}  // namespace sonora::elastic
