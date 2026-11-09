// SPDX-License-Identifier: GPL-3.0-or-later
//
// Import Session Data (ADR-0123): tracks, with their material and settings,
// taken from another Sonora session into the open one.
//
// A pure planner. It reads two snapshots and returns the operations that add
// the tracks and the list of audio files to bring in; it opens no file and
// writes nothing, so it is tested without Qt or a disk and its result
// replays on every client as ordinary operations (ADR-0009). The interface's
// worker copies `ImportPlan::copies` first and applies `ImportPlan::ops`
// only when every copy succeeded: a session never names a file outside its
// own folder (ADR-0018), and an import that fails half-way applies nothing.
//
// Never on the audio thread: it allocates. No engine, Qt or platform header.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/model/operations.h"

namespace sonora::session_files {

// What the operator ticked in the Import panel. Every flag is per request,
// not per track, as Pro Tools' Import Session Data does it: one set of
// switches over the chosen tracks.
struct SessionImportRequest {
    // Source track ids, in the order the new tracks take after the
    // destination's last track. Ids the source does not have refuse the
    // request rather than being skipped, so a stale list is not half-applied.
    std::vector<model::Id> tracks;
    // The active playlist's regions, fades and crossfades. Off brings empty
    // tracks with the same name, channels and (with `mix`) settings.
    bool material = true;
    // Fader, pan and mute. Solo is never carried: a soloed import would
    // silence the rest of the session the moment it lands.
    bool mix = true;
    // Inserts with their parameter values; their state files are not carried
    // (reported as `approximated:`), because a state blob names files in the
    // other session's "Plugin State/".
    bool inserts = true;
    // Volume and pan automation lanes.
    bool automation = true;
    // The source's markers. Off by default: markers belong to the session's
    // timeline, and importing a track is rarely meant to bring another song's.
    bool markers = false;
    // Frames added to every region and automation point (the playhead for
    // "at playhead"); 0 keeps the source's times.
    model::Frames offset = 0;
    // The source session's folder, absolute: `FileCopy::source_path` is
    // under it. The source is opened read-only and never written.
    std::string source_folder;
    // The format version the source file declared (model::file_format_version).
    // Here because the planner sees a snapshot, not the bytes, and a newer
    // file may hold what an older reader would silently drop.
    std::int64_t source_format_version = 0;
    // A take is being recorded in the destination. The caller knows; the
    // snapshot does not. An import then would race the take's own sources.
    bool take_open = false;
    // Whether an absolute path names a readable file. Injected so the planner
    // stays pure and a test can make a file missing; empty means "assume
    // readable" and leaves the check to the copier, which checks every file
    // before copying any.
    std::function<bool(const std::string& absolute_path)> source_readable;
};

// One audio file to bring into the destination before `ops` is applied.
struct FileCopy {
    // Absolute, under SessionImportRequest::source_folder.
    std::string source_path;
    // Relative to the destination session folder, under kAudioDir; a name
    // already there gets " 2", " 3"... before the extension, never an
    // overwrite of a file another source names.
    std::string dest_path;
    // The id the destination's new source takes in `ops`, so the copier
    // can say which source a failed copy would have left without its file.
    model::Id source = 0;
};

// What an import does, built before anything is written.
struct ImportPlan {
    // One Compound labelled "Import Session Data": one undo step. Its ids
    // run strictly upward from the destination's next_id in a fixed order
    // (sources, then each track and what it holds, in request order), so the
    // same inputs give the same bytes and a test can compare them.
    model::Compound ops;
    // Files to copy; a source already in the destination with the same
    // non-empty umid is reused and needs none.
    std::vector<FileCopy> copies;
    // What the request named and v1 does not import, one line each, in the
    // words sonora-convert uses: MIDI and instrument tracks, sends, buses,
    // VCAs, groups.
    std::vector<std::string> dropped;
    // What arrives changed: an insert without its state file, a track whose
    // output bus does not exist here and lands on the Master.
    std::vector<std::string> approximated;
};

// Why an import was refused, before any file was copied. The interface shows
// it as a held notice: the operator must read which file or why.
struct ImportRefusal {
    enum class Reason : std::uint8_t {
        None,
        InvalidRequest,   // no track, an id the source lacks, no source folder
        NewerFormat,      // source written by a newer Sonora
        MissingFile,      // `files` names each one
        SampleRate,       // source rate differs; v1 does not resample
        TakeOpen,         // a take is being recorded here
        TooLarge,         // past model::kMaxCompoundMembers in one step
    };
    Reason reason = Reason::None;
    std::string message;               // one line, for the notice
    std::vector<std::string> files;    // the files at fault, source-relative
};

// The plan that imports `request.tracks` of `source` into `dest`. True with
// `plan` filled; false with `refusal` saying why and `plan` left empty. A
// request whose Compound would pass model::kMaxCompoundMembers is refused
// (TooLarge, naming how many of the requested tracks fit) rather than split,
// so one import is always one undo step; the interface imports in batches.
bool plan_session_import(const model::Snapshot& dest, const model::Snapshot& source,
                         const SessionImportRequest& request, ImportPlan& plan,
                         ImportRefusal& refusal);

// One row of the Import panel's track list.
struct ImportableTrack {
    model::Id id = 0;
    std::string name;
    model::TrackKind kind = model::TrackKind::Audio;
    std::uint16_t channels = 1;
    std::size_t regions = 0;     // on the active playlist
    model::Frames length = 0;    // end of the last region, source frames
    // False for what v1 does not import (MIDI, instrument), with `why`, so
    // the panel shows the track greyed out instead of hiding it.
    bool importable = true;
    std::string why;
};

// The source's tracks in edit order, for the operator to tick.
std::vector<ImportableTrack> list_importable_tracks(const model::Snapshot& source);

}  // namespace sonora::session_files
