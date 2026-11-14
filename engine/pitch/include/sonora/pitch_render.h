// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `pitch` verb of sample.edit (time.pitch, ADR-0143): the notes of one
// region are shifted into a copy, "Audio Files/<track>_pitch_NN.wav", and the
// region is pointed at it by ONE Compound (AddSource, SetRegionSources), so
// one undo puts the original back. The take is only ever read.
//
// The control thread's: it reads and writes whole files and allocates. Never
// the audio thread; of the session it reads only whether a take is open.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sonora/api/pitch.h"
#include "sonora/model/model.h"
#include "sonora/model/replication.h"
#include "sonora/pitch/shifter.h"

namespace sonora {

class Session;

struct PitchRenderRequest {
    model::Id region = 0;
    // The selection, in timeline frames, inside the region; every note lies
    // within it.
    model::Frames from = 0;
    model::Frames to = 0;
    // Note frames are relative to the region's start (contracts/api/pitch.h).
    api::PitchEditSpec spec;
};

// The longest copy one pitch edit renders: the whole span is held in memory.
inline constexpr model::Frames kMaxPitchRenderFrames = 16'000'000;
// Source frames kept either side of the region in the copy, as sample.edit.
inline constexpr double kPitchHandleSeconds = 2.0;

// Bytes free where the copy goes. A seam, so a test can fill the disk.
using FreeBytes = std::function<std::uint64_t(const std::string& dir)>;
// The file system's answer; when it cannot say, no limit.
std::uint64_t free_bytes_on_disk(const std::string& dir);

// Refused, saying why, while a take is recording or closing, for a note list
// outside the region or the selection, or overlapping, for a copy longer than
// kMaxPitchRenderFrames, when the disk has not room for the copy, and when
// the shifter refuses or returns another length. On any refusal the document
// is unchanged and no new file is left behind; `updates` gets the one update
// for every replica on success.
bool render_pitch(model::Document& document, const Session& session,
                  const std::string& session_dir, const PitchRenderRequest& request,
                  pitch::PitchShifter& shifter, const FreeBytes& free_bytes,
                  std::vector<model::ReplicationUpdate>& updates, std::string& error);

// Whether this engine was built with a shifter (Rubber Band found by
// pkg-config). Without one the `pitch` capability is off.
bool pitch_shifter_built();
// The built shifter, or null without one.
std::unique_ptr<pitch::PitchShifter> make_pitch_shifter();

// sample.edit's entry: the built shifter and the disk's own free space.
bool pitch_sample_edit(model::Document& document, const Session& session,
                       const std::string& session_dir, const PitchRenderRequest& request,
                       std::vector<model::ReplicationUpdate>& updates, std::string& error);

}  // namespace sonora
