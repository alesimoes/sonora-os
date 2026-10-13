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

// Reads and parses `file`. On failure `why` says where in the file.
bool read_session_file(const QString& file, std::string& bytes, model::Loaded& out,
                       std::string& why);

}  // namespace sonora::ui
