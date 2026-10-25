// SPDX-License-Identifier: GPL-3.0-or-later
//
// A session folder on disk (session-format.md, "Saving"; ADR-0015 §3, §6):
// the session file written atomically, the MIDI sequence files it names
// written before it, and the sequence files no kept session file names
// deleted after it.
//
// The engine's saver thread (engine/control session_saver.h) is the one
// writer in the product; it decides when a save may touch the disk, which
// this does not know. The interface's tests use this to stand in for the
// engine. common/model produces the bytes and does no I/O; this is the I/O.
//
// Not thread-safe: one saver owns one SessionFolder. Never on the audio
// thread: it allocates, blocks on the medium and touches the filesystem.
#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/model/sequence_file.h"

namespace sonora::session_files {

inline constexpr const char* kSequenceDir = "MIDI Sequences";
inline constexpr const char* kBackupsDir = "Session File Backups";

// The session folder of a session file: its own folder, or, for a file in
// "Session File Backups/", the folder above it. Sequence files live there
// whichever kept file names them.
std::string session_folder_of(const std::string& session_file);

class SessionFolder {
public:
    // Writes `bytes`, a canonical session file of `document`
    // (model::serialize), to `session_file`, an absolute path ending in
    // ".sonora", in this order:
    //   1. a file of a newer format version there is refused and left as it
    //      is; one of an older version is first copied, once, to
    //      "Session File Backups/<Name>.v<old>.sonora";
    //   2. each sequence file `bytes` names that is not known to be on disk
    //      is encoded from `document`, written to "<sha>.msq.tmp", synced and
    //      renamed to "<sha>.msq", then "MIDI Sequences/" is synced once;
    //   3. `bytes` to "<file>.tmp", synced, renamed over `session_file`, and
    //      the folder synced.
    // False, with `why`, when the save did not happen: the previous session
    // file is as it was, and a session folder this call created is removed
    // again so a retry finds the name free. A sequence `bytes` names that
    // `document` does not hold fails the save by name. True once the rename
    // has landed; `caution` then says what could not be made durable (the
    // folder would not sync), and empty means the save is durable.
    bool save(const std::string& session_file, const std::string& bytes,
              const model::Snapshot& document, std::string& why, std::string& caution);

    // Deletes each sequence file no kept session file names (this one, the
    // autosave, each copy in "Session File Backups/"); nothing while a kept
    // file cannot be read, and `caution` then says which. How many went.
    // Call only after a durable save: with the folder unsynced, the old
    // session file can come back after a power cut, and what it names must
    // still be there.
    int clean(std::string& caution);

    // The folder the last save entered; empty before the first.
    const std::string& folder() const { return folder_; }
    // Kept files parsed since the folder was entered, for the tests: each
    // is read once, when first seen.
    std::size_t kept_reads() const { return kept_reads_; }
    // Sequence files written since construction, for the tests.
    std::size_t sequences_written() const { return sequences_written_; }

private:
    // Entering a folder: an interrupted save (".msq.tmp") is deleted, and
    // every kept file's references read.
    void enter(const std::string& folder);
    void refresh_kept();
    // Whether "<sha>.msq" is already there with exactly the bytes its name
    // hashes: a file renamed into place only after it was synced.
    bool verified_on_disk(const std::string& sha);

    std::string folder_;
    model::SequenceDigests digests_;
    model::SequenceFileSet files_;
    std::set<std::string> known_;   // the kept files files_ holds a set for
    std::size_t kept_reads_ = 0;
    std::size_t sequences_written_ = 0;
};

}  // namespace sonora::session_files
