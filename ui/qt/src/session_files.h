// SPDX-License-Identifier: GPL-3.0-or-later
//
// Session files on disk, for File > Save and Open in the interface.
//
// common/model turns a Snapshot into canonical bytes and back and does no
// I/O (session_file.h); this is the I/O. It is the interface's for now,
// because the engine has no session commands yet. When it does
// (session-format.md, "Saving": one saver thread in engine/control, backups
// rotated, autosave) Save and Open become commands sent to the engine and
// this file goes; until then it writes the way the spec asks of that saver
// -- a temporary file, flushed and synced, renamed over the old one, then
// the folder synced -- so a power cut leaves the old file or the new one.
//
// What it does not do yet, and nothing claims it does: rotated backups in
// "Session File Backups/" (it keeps only the pre-migration copy), autosave,
// and offering a newer autosave at open.
//
// The layout is the spec's: <dir>/<Name>/<Name>.sonora, one folder per
// session, all under one sessions folder. The interface lists that folder
// and nothing else; there is no file browser on the appliance.
#pragma once

#include <QString>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "sonora/model/sequence_file.h"
#include "sonora/model/session_file.h"

namespace sonora::ui {

// $SONORA_SESSIONS_DIR, else ~/Sessions: /home is what the persistence
// partition keeps across boots (ADR-0005).
QString default_sessions_dir();

// Why `name` cannot name a session folder, or empty when it can. Refused
// rather than rewritten, so the folder is called what the session is.
std::string session_name_problem(const QString& name);

// <dir>/<name>/<name>.sonora
QString session_file_path(const QString& dir, const QString& name);

// The first of "name", "name 2", "name 3".. with no folder in `dir`.
QString unused_session_name(const QString& dir, const QString& name);

struct SessionEntry {
    QString name;  // the folder's name
    QString file;  // its .sonora file
};

// Every <dir>/<Name>/<Name>.sonora, sorted by name. A folder without its
// session file is not listed.
std::vector<SessionEntry> list_sessions(const QString& dir);

// Atomically replaces `file` with `bytes`, creating its folder. A `file` of
// an older format version is first copied, once, to "Session File
// Backups/<Name>.v<old>.sonora" (session-format.md, "Versioning"); a `file`
// of a newer format version is refused and left untouched. On
// failure `why` says what failed, and a folder this call created is removed
// again, so a retry finds the name free. Once the new file has replaced the old one
// the save has happened and this returns true; `caution` is then empty, or
// says what could not be made durable (a folder that would not sync).
bool write_session_file(const QString& file, const std::string& bytes, std::string& why,
                        std::string& caution);

// Reads and parses `file`, and each sequence file it names from its
// folder's "MIDI Sequences/". On failure `why` says where in the file. A
// sequence file that is not there or does not verify is not a failure: it
// is in `out.missing_sequences` (ADR-0015 O2).
bool read_session_file(const QString& file, std::string& bytes, model::Loaded& out,
                       std::string& why);

// A session's "MIDI Sequences/" (ADR-0015 §3, §6), for File > Save and
// Open. Each sequence a save names is written before the session file:
// "<sha>.msq.tmp", synced, renamed to "<sha>.msq", and one sync of the
// folder for the whole save, so a session file never names a sequence
// that is not durable. A sequence file no kept session file names (this
// one, and each copy in "Session File Backups/") is derived state, and
// clean() deletes it without asking.
//
// What is on disk and what each kept file names are held in memory between
// saves: each kept file is parsed once, when first seen, and one that has
// gone is dropped. The session folder is the one a backup or the autosave
// belongs to, never "Session File Backups/" itself. Still the interface's
// until the engine has a saver thread (session-format.md, "Saving"); that
// saver also paces its syncs behind the disk thread's checkpoints while a
// take is open (ADR-0015 §6), which this one cannot see, so the interface
// only holds back cleanup while it records.
class MidiSequenceFolder {
public:
    // `session_file`'s folder, as an open found it: an interrupted write
    // (".msq.tmp") is deleted, and every kept file's references read.
    // `verified` are the sequence files the open read and checked.
    void open(const QString& session_file, const std::set<std::string>& verified);
    // Every sequence `s` names that is not on disk, written and synced, then
    // the folder synced once. False, with `why` naming the file, when one
    // could not be: then the session file must not be written.
    bool write(const QString& session_file, const model::Snapshot& s, std::string& why);
    // The session file now holds `s`: what it names is kept, from `s`
    // itself, without reading the file back.
    void saved(const QString& session_file, const model::Snapshot& s);
    // Deletes each sequence file no kept file names; nothing while a kept
    // file cannot be read (`caution` then says which). How many went.
    // Call only once the save is durable.
    int clean(std::string& caution);
    // What the saves of this window have hashed, so serialize() hashes only
    // what changed since.
    model::SequenceDigests& digests() { return digests_; }
    // Kept files parsed since the folder was entered, for the tests.
    std::size_t kept_reads() const { return kept_reads_; }

private:
    void enter(const QString& session_file);
    void refresh_kept();

    QString folder_;   // the session folder, empty before the first
    model::SequenceDigests digests_;
    model::SequenceFileSet files_;
    std::set<QString> known_;   // the kept files files_ holds a set for
    std::size_t kept_reads_ = 0;
};

// The session folder of a session file: its own folder, or, for a file in
// "Session File Backups/", the folder above it.
QString session_folder_of(const QString& file);

}  // namespace sonora::ui
