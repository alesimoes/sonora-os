// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cross-session paste (ADR-0142): the clipboard's audio brought into the
// session it is pasted into, by Import Session Data's rules (ADR-0123).
//
// A pure planner. It reads the clipboard's sources, the destination's
// snapshot and the folder the clipboard was copied from, and returns how
// each clipboard source id maps onto the destination's pool, the AddSource
// members for the ones it lacks and the files to copy for them. It opens no
// file; whether a file is readable is injected. The caller copies first and
// puts `ops` ahead of the paste's own members only when every copy
// succeeded (bring_in_sources), because a session never names a file
// outside its own folder (ADR-0018). AddSource is outside undo, so undoing
// the paste removes its regions and leaves the audio in the pool.
//
// Never on the audio thread: it allocates. No engine, Qt or platform header.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/model/operations.h"
#include "sonora/session_files/session_import.h"
#include "sonora/session_files/session_import_copy.h"

namespace sonora::session_files {

struct PasteSourcesRequest {
    // The clipboard's sources as they were in the origin's pool at copy,
    // every lane's together; an id named twice is planned once.
    std::vector<model::Source> sources;
    // The origin session's folder, absolute, as the clipboard remembered it.
    // Empty when the clipboard does not know it: a paste that needs a file
    // from there is then refused.
    std::string origin_folder;
    // The origin's sample rate; 0 when not known. v1 does not resample, so a
    // known rate that differs refuses the paste as Import Session Data does.
    std::uint32_t origin_sample_rate = 0;
    // Whether an absolute path names a readable file. Empty means "assume
    // readable" and leaves the check to the copier.
    std::function<bool(const std::string& absolute_path)> source_readable;
};

struct PasteSourcesPlan {
    // Clipboard source id -> the destination's source id, for every source
    // of the request: itself when the destination already holds it (a paste
    // within the session), a source with the same non-empty umid, or the id
    // of a new AddSource in `ops`.
    std::map<model::Id, model::Id> remap;
    // One AddSource per source the destination lacks, ids strictly upward
    // from the destination's next_id in request order. The paste's own
    // members go after them.
    std::vector<model::Member> ops;
    // The files the AddSources name, into the destination's Audio Files/;
    // a name already there takes " 2", " 3"... before the extension.
    std::vector<FileCopy> copies;
    // The id the paste's own members start from: the destination's next_id
    // past the ids `ops` took.
    model::Id next_id = 1;
    // Empty when the paste can go ahead; otherwise why not, in the
    // operator's words, ending "nothing was pasted" (a held notice).
    std::string problem;
};

// The plan for pasting `request.sources` into `dest`. False with
// `plan.problem` set, and nothing else in `plan`, when it is refused.
bool plan_paste_sources(const model::Snapshot& dest, const PasteSourcesRequest& request,
                        PasteSourcesPlan& plan);

// Copies the plan's files; false, with `error`, if any failed. On failure
// the copier has removed what it wrote (perform_copies does).
using PasteCopyFn = std::function<bool(const std::vector<FileCopy>& copies, std::string& error)>;

// The ordering ADR-0142 requires: `copy` runs over every file first, and
// `plan.ops` is appended to `out` only when it succeeded. False, with `out`
// untouched and `error` set, when a copy failed. A plan with no copy does
// not call `copy`.
bool bring_in_sources(const PasteSourcesPlan& plan, const PasteCopyFn& copy,
                      std::vector<model::Member>& out, std::string& error);

// The PasteCopyFn of a real paste: perform_copies (ADR-0123 decision 3)
// into `dest_folder`, all or nothing, on the caller's worker thread.
PasteCopyFn disk_copier(const std::string& dest_folder, const std::atomic<bool>& cancel,
                        const CopyOptions& options = CopyOptions{});

}  // namespace sonora::session_files
