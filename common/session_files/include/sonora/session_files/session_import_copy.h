// SPDX-License-Identifier: GPL-3.0-or-later
//
// Import Session Data's copier (ADR-0123 decision 3): the audio files a plan
// names, brought into the destination's "Audio Files/" before its Compound
// is applied, so the session never names a file outside its own folder
// (ADR-0018).
//
// All or nothing on disk: every file is checked before any is written, and a
// failure or a cancel deletes every file this call wrote, so the caller
// applies `ImportPlan::ops` only when it returns Done. A hard link where the
// two sessions share a filesystem (instant, no space), else a copy synced to
// the medium before it takes its name. Never overwrites: a name already
// there fails the import rather than replacing another source's audio.
//
// Runs on the interface's import worker. Never on the audio thread: it
// allocates and blocks on the medium for as long as a USB stick takes.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "sonora/session_files/session_import.h"

namespace sonora::session_files {

struct CopyProgress {
    std::size_t files_done = 0;
    std::size_t files_total = 0;
    std::uint64_t bytes_done = 0;
    std::uint64_t bytes_total = 0;
};

// Called on the copier's thread after each file and each chunk of a copy:
// the worker forwards it to the panel's progress bar.
using CopyProgressFn = std::function<void(const CopyProgress&)>;

enum class CopyOutcome : std::uint8_t { Done, Failed, Cancelled };

struct CopyReport {
    CopyOutcome outcome = CopyOutcome::Done;
    // One line for the held notice when not Done.
    std::string message;
    // The source file at fault, absolute; empty for a cancel.
    std::string file;
    std::size_t linked = 0;   // brought in as hard links
    std::size_t copied = 0;   // brought in as synced copies
};

struct CopyOptions {
    // Off copies every file even on one filesystem: what a test of the copy
    // path, and a stick with no hard links, take.
    bool hard_links = true;
    // The size of one read and write; `cancel` is looked at between chunks.
    std::size_t chunk_bytes = std::size_t{1} << 20;
};

// Brings `plan.copies` into `session_folder` (absolute), creating its
// "Audio Files/" if needed. `cancel` is read before each file and each
// chunk; set from another thread, it stops the copy and deletes what was
// written. Done only when every file is in place and its folder synced.
CopyOutcome perform_copies(const ImportPlan& plan, const std::string& session_folder,
                           const std::atomic<bool>& cancel, const CopyProgressFn& progress,
                           CopyReport& report, const CopyOptions& options = CopyOptions{});

}  // namespace sonora::session_files
