// SPDX-License-Identifier: GPL-3.0-or-later
//
// A session folder on disk (session-format.md, "Saving"; ADR-0015 §3, §6):
// the session file written atomically, the MIDI sequence files it names
// written before it, and the sequence files no kept session file names
// deleted after it; and the plugin state files a FlushPluginState hands it,
// before any session file names them.
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
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/model/sequence_file.h"

namespace sonora::session_files {

inline constexpr const char* kSequenceDir = "MIDI Sequences";
inline constexpr const char* kBackupsDir = "Session File Backups";
inline constexpr const char* kPluginStateDir = "Plugin State";
inline constexpr const char* kAudioDir = "Audio Files";
inline constexpr const char* kMidiFilesDir = "MIDI Files";

// The session file's rotated copies a save keeps (session-format.md,
// "Saving", step 5): ADR-0015 §6's bound on sequence files counts on 20.
inline constexpr std::size_t kDefaultBackups = 20;

// One plugin state to keep beside the session (ADR-0012 §6.1): a StateBlob,
// opaque here, and the digest that names its file.
struct StateFile {
    std::string sha256;   // sha256_hex(bytes)
    std::string bytes;
};

// The session folder of a session file: its own folder, or, for a file in
// "Session File Backups/", the folder above it. Sequence files live there
// whichever kept file names them.
std::string session_folder_of(const std::string& session_file);

// Where the autosave of `session_file` goes: "<folder>/Session File
// Backups/<session stem>.autosave.sonora". Per session file, so two files in
// one folder do not share one; a rotated backup of "Mix" has Mix's autosave.
// The interface finds it at open by the same rule.
std::string autosave_file_of(const std::string& session_file);

// Whether `name` is one of the rotated backups a save of a session file
// whose stem is `stem` keeps: "<stem>.<yyyymmdd>-<hhmmss>.sonora", or with
// "-<n>" before ".sonora" when that second was taken. The only names
// pruning may delete: never the autosave, a pre-migration copy
// ("<stem>.v<n>.sonora") or another session's backups.
bool rotated_backup_name(const std::string& name, const std::string& stem);

// Deletes what an interrupted Save As copy left (ADR-0018 §5, step 4):
// "<name>.part" regular files in the folder's "Audio Files/" and "MIDI
// Files/". No session file names one. A symlink is never followed or
// removed. How many went.
int sweep_partial_copies(const std::string& folder);

// What a Save As put in the new folder, for the reply and the tests.
struct SaveAsReport {
    std::size_t linked = 0;           // files given a second name (same filesystem)
    std::size_t copied = 0;           // files copied (another filesystem, or link refused)
    std::uint64_t copied_bytes = 0;
    // Sources the document names that were not in the old folder either:
    // offline there, offline in the new one.
    std::vector<std::string> missing;
};

class SessionFolder {
public:
    // Syncs a directory; true on success. A test passes one that fails, to
    // reach the paths a medium that will not sync takes.
    using SyncDirectory = bool (*)(const std::string& path);
    // Null: platform::sync_directory.
    explicit SessionFolder(SyncDirectory sync_directory = nullptr);

    // Writes `bytes`, a canonical session file of `document`
    // (model::serialize), to `session_file`, an absolute path ending in
    // ".sonora", in this order:
    //   1. a file of a newer format version there is refused and left as it
    //      is; one of an older version is first copied, once, to
    //      "Session File Backups/<Name>.v<old>.sonora";
    //   2. each sequence file `bytes` names that is not known to be on disk
    //      is encoded from `document`, written to "<sha>.msq.tmp", synced and
    //      renamed to "<sha>.msq", then "MIDI Sequences/" is synced once;
    //   3. each plugin state `bytes` names that is not in the folder's
    //      "Plugin State/" with its bytes is copied there from
    //      `state_source`'s (the engine's --dir, where a FlushPluginState
    //      wrote it) the same way, then "Plugin State/" is synced once
    //      unless every one is already known durable: a session never points
    //      outside its own folder;
    //   4. `bytes` to "<file>.tmp", synced, renamed over `session_file`, and
    //      the folder synced;
    //   5. the bytes the file held before, when they differ, kept as
    //      "Session File Backups/<stem>.<yyyymmdd-hhmmss>.sonora" (UTC) the
    //      same way, and the rotated backups of this stem past the newest
    //      backups() deleted (prune_backups()). Not for a file in "Session
    //      File Backups/" itself. A backup that cannot be kept is a
    //      `caution`: the session file is already written.
    // False, with `why`, when the save did not happen: the previous session
    // file is as it was, and a session folder this call created is removed
    // again so a retry finds the name free. A sequence `bytes` names that
    // `document` does not hold fails the save by name, and so does a plugin
    // state found in neither folder, unless the file being replaced already
    // names it (its state was lost before, and the operator told at open).
    // Refused for a rotated-backup name in "Session File Backups/", which
    // pruning deletes; `why` points to Save As. True once the rename
    // has landed; `caution` then says what could not be made durable (the
    // folder would not sync), and empty means the save is durable.
    bool save(const std::string& session_file, const std::string& bytes,
              const model::Snapshot& document, std::string& why, std::string& caution,
              const std::string& state_source = std::string());

    // Deletes each sequence file no kept session file names (this one, the
    // autosave, each copy in "Session File Backups/"); nothing while a kept
    // file cannot be read, and `caution` then says which. How many went.
    // Call only after a durable save: with the folder unsynced, the old
    // session file can come back after a power cut, and what it names must
    // still be there.
    int clean(std::string& caution);

    // How many rotated backups save() keeps; 0 keeps none and writes none.
    void set_backups(std::size_t keep) { backups_ = keep; }
    std::size_t backups() const { return backups_; }
    // Seconds since the epoch, for the backups' names; the tests set one.
    using Clock = long long (*)();
    void set_clock(Clock clock) { clock_ = clock; }

    // Deletes the rotated backups of `stem` (rotated_backup_name()) in
    // "<folder>/Session File Backups/" past the newest `keep`, newest by
    // name. Only regular files are counted or deleted: a symlink, even one
    // named as a backup, is never followed or removed, and nothing is
    // deleted when "Session File Backups" is itself a symlink (`caution`
    // then says so). How many went.
    int prune_backups(const std::string& folder, const std::string& stem, std::size_t keep,
                      std::string& caution);

    // The autosave (session-format.md, "Autosave"): `document` serialised
    // here, off the caller's thread, with the save_generation after the one
    // `session_file` declares (so the interface offers it at open only
    // while it is newer than the file), and written to
    // autosave_file_of(session_file) by save()'s steps 2-4, the plugin
    // states it names copied from `state_source`. Bytes equal to the
    // autosave already there write nothing. Refused when "Session File
    // Backups" is a symlink: an autosave is never written outside the
    // session's folder. An autosave there that this saver has not written
    // since entering the folder (a crash's) is first moved to
    // "<stem>.autosave.recovered-<UTC stamp>.sonora", named in `caution` and
    // set_aside(). False with `why` when nothing was written.
    bool autosave(const std::string& session_file, const model::Snapshot& document,
                  const std::string& written_by, const std::string& state_source, std::string& why,
                  std::string& caution);

    // Save As and Save Copy In (ADR-0018 §5 and its update of 2026-10-27):
    // a new, self-contained session folder for `session_file`, made from
    // `source_folder`, which is only read. Refused when the new folder
    // exists and is not empty, is the source folder, or holds it or is held
    // by it. With `media`, every file of the source's "Audio Files/" and
    // "MIDI Files/" and every source the document names is given a second
    // name there (platform::hard_link) or, where that is refused, copied
    // through "<name>.part", synced and renamed, after a check that the
    // copies fit; each new directory is synced once. Then save() writes the
    // sequence files, the plugin states (from `source_folder`) and, last,
    // the session file, so a folder without one is an interrupted Save As.
    // False with `why`: the folder this call made is removed again and the
    // source is as it was.
    bool save_as(const std::string& session_file, const std::string& bytes,
                 const model::Snapshot& document, const std::string& source_folder, bool media,
                 std::string& why, std::string& caution, SaveAsReport* report = nullptr);

    // Writes each state to "<folder>/Plugin State/<sha256>.state"
    // (model::plugin_state_file) through "<name>.tmp", synced and renamed,
    // then syncs "Plugin State/" once, so a session file saved afterwards
    // never names a state the medium does not hold (ADR-0012 §6.1). A file
    // already there with the bytes its name hashes is not written again;
    // states are append-only and nothing here deletes one. False, with
    // `why`, at the first state not made durable (one whose digest is not
    // its bytes' included); the ones before it stay, which is harmless,
    // since no document names them yet. Independent of save()'s folder.
    bool write_states(const std::string& folder, const std::vector<StateFile>& states, std::string& why);
    // State files written since construction, for the tests.
    std::size_t states_written() const { return states_written_; }

    // The folder the last save entered; empty before the first.
    const std::string& folder() const { return folder_; }
    // The name the last autosave() moved a foreign autosave to, or empty.
    const std::string& set_aside() const { return set_aside_; }
    // Kept files parsed since the folder was entered, for the tests: each
    // is read once, when first seen.
    std::size_t kept_reads() const { return kept_reads_; }
    // Sequence files written since construction, for the tests.
    std::size_t sequences_written() const { return sequences_written_; }

private:
    // Entering a folder: an interrupted save (".msq.tmp", ".state.tmp") is
    // deleted, and every kept file's references read.
    void enter(const std::string& folder);
    // Deletes "Plugin State/*.state.tmp" in `folder`, once per folder.
    void sweep_states(const std::string& folder);
    // Creates "<folder>/Plugin State/", its entry synced when new.
    bool make_state_dir(const std::string& folder, std::string& why);
    // Syncs "<folder>/Plugin State/" unless every one of `paths` is in
    // durable_states_, then adds them: a file found there, or renamed there
    // before a sync that failed, is not yet a durable entry.
    bool sync_states(const std::string& folder, const std::vector<std::string>& paths, std::string& why);
    void refresh_kept();
    // Whether "<sha>.msq" is already there with exactly the bytes its name
    // hashes: a file renamed into place only after it was synced.
    bool verified_on_disk(const std::string& sha);

    // Step 5 of save(): `old` kept as the newest rotated backup of `file`.
    bool keep_backup(const std::string& file, const std::string& old, std::string& caution);
    // A file the media copy wrote, synced and renamed into place.
    bool copy_synced(const std::string& from, const std::string& to, std::string& why);

    SyncDirectory sync_directory_;
    std::size_t backups_ = kDefaultBackups;
    Clock clock_ = nullptr;
    std::string folder_;
    model::SequenceDigests digests_;
    model::SequenceFileSet files_;
    std::set<std::string> known_;   // the kept files files_ holds a set for
    std::size_t kept_reads_ = 0;
    std::size_t sequences_written_ = 0;
    std::size_t states_written_ = 0;
    std::set<std::string> durable_states_;   // state files a folder sync has covered
    std::set<std::string> autosaves_written_;   // autosaves this saver wrote since entering folder_
    std::string set_aside_;
    std::set<std::string> swept_;            // folders whose "Plugin State/" was swept
};

}  // namespace sonora::session_files
