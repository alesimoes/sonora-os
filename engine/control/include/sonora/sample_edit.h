// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sample-level editing (Pro Tools' Pencil repair, Logic's Audio File Editor;
// ADR-0052): a short stretch of a region is redrawn, silenced, reversed,
// inverted, gained, normalised or faded, as a destructive process that never
// touches the take. The region's span, with a handle either side, is copied
// to a new file in "Audio Files/" with the stretch processed, and the region
// points at the copy; one undo step puts it back on the original.
//
// The control thread's: it reads and writes files and allocates. Never the
// audio thread; it touches nothing of the session but whether a take is open.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/model/replication.h"
#include "sonora/session.h"

namespace sonora {

enum class SampleVerb { Repair, Draw, Silence, Reverse, Invert, Gain, Normalize, FadeIn, FadeOut };

struct SampleEditRequest {
    SampleVerb verb = SampleVerb::Repair;
    model::Id region = 0;
    model::Frames from = 0;   // timeline frames, [from, to), inside the region
    model::Frames to = 0;
    int channel = -1;         // a region channel, or -1 for all of them
    float db = 0.0f;          // Gain: the change; Normalize: the peak it reaches
    // Draw: (timeline frame, level in -1..1), strictly increasing, joined by
    // straight lines; the stretch outside the first and last point holds.
    std::vector<std::pair<model::Frames, float>> points;
};

// The longest stretch one edit processes: it is held in memory, one channel
// at a time. The longest a Repair redraws: it is for clicks.
inline constexpr model::Frames kMaxSampleEditFrames = 16'000'000;
inline constexpr model::Frames kMaxRepairFrames = 2048;
// Source frames kept either side of the region in the copy, so a crossfade
// or a later trim has material to draw on.
inline constexpr double kSampleEditHandleSeconds = 2.0;

// "VERB REGION FROM TO CHANNEL [PARAM]": VERB is repair, draw, silence,
// reverse, invert, gain, normalize, fade_in or fade_out; FROM and TO are
// timeline frames; CHANNEL is a number from 1 or "all"; PARAM is the dB of
// gain and normalize (normalize without one reaches 0 dB), and for draw
// "FRAME:LEVEL,FRAME:LEVEL,...". On failure `why` says what is wrong.
bool parse_sample_edit(const std::string& text, SampleEditRequest& out, std::string& why);

// Processes buf[lead, lead + count) in place. buf holds `lead` frames of
// context before the stretch and `tail` after it (fewer at a file's ends);
// Repair reads them to match the slope into and out of the stretch, and
// nothing outside the stretch changes. `first` is the stretch's timeline
// frame, for Draw's points. False, saying why, for a stretch the verb
// refuses: a Repair longer than kMaxRepairFrames, a Draw with no points,
// Normalize on silence.
bool process_samples(const SampleEditRequest& request, std::vector<float>& buf,
                     std::size_t lead, std::size_t count, std::size_t tail,
                     model::Frames first, std::string& why);

// Refused, saying why, while a take is recording or closing. Otherwise the
// copy, then one Compound labelled "Sample Edit: <verb>" (AddSource and
// SetRegionSources) applied to `document`. `updates` gets the one update to
// send every replica. On any failure the document is unchanged and the new
// file is removed unless the model refuses after it was written, when it stays
// in "Audio Files/" but out of the pool, and `error` says so.
bool sample_edit(model::Document& document, const Session& session,
                 const std::string& session_dir, const SampleEditRequest& request,
                 std::vector<model::ReplicationUpdate>& updates, std::string& error);

}  // namespace sonora
