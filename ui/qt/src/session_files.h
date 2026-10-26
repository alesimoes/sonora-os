// SPDX-License-Identifier: GPL-3.0-or-later
//
// Session files on disk, as File > Save and Open in the interface see them.
//
// The interface reads and never writes: File > Save hands the bytes to the
// engine, whose saver thread writes the session folder (ModelAction::Save,
// ADR-0015 update of 2026-10-25), so only the engine writes or deletes in a
// session's "MIDI Sequences/". File > Open reads the session file and the
// sequence files it names here, because the engine reads nothing from the
// interface's folder (ModelAction::Load carries the bytes, and the file's
// path only so the engine records into its folder, ADR-0018 §2).
//
// The layout is the spec's: <dir>/<Name>/<Name>.sonora, one folder per
// session, all under one sessions folder. The interface lists that folder
// and nothing else; there is no file browser on the appliance.
#pragma once

#include <QString>
#include <cstdint>
#include <string>
#include <vector>

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

struct RecordingEntry {
    QString name;    // the folder's name
    QString folder;  // its path
};

// Every folder of `dir` with a take in its "Audio Files/" and no session
// file of any name, sorted by name: an untitled session the engine was
// recording when it stopped, or a Save As cut short (ADR-0018 §3, §5).
std::vector<RecordingEntry> list_unsaved_recordings(const QString& dir);

// Reads and parses `file`, and each sequence file it names from its
// session folder's "MIDI Sequences/". On failure `why` says where in the
// file. A sequence file that is not there or does not verify is not a
// failure: it is in `out.missing_sequences` (ADR-0015 O2).
bool read_session_file(const QString& file, std::string& bytes, model::Loaded& out,
                       std::string& why);

// The session folder of a session file: its own folder, or, for a file in
// "Session File Backups/", the folder above it.
QString session_folder_of(const QString& file);

}  // namespace sonora::ui
