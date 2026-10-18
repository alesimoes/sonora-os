// SPDX-License-Identifier: GPL-3.0-or-later
//
// The native session file (brain/specs/session-format.md): a Snapshot to
// canonical JSON bytes and back. No I/O: serialize() returns bytes and
// deserialize() takes them. Writing them durably is engine/control's job,
// through common/platform (ADR-0009 §5).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sonora/model/model.h"

namespace sonora::model {

// The format version this program writes, and the newest it reads.
// 2: plugin inserts on tracks (ADR-0012 §6.2).
constexpr std::int64_t kFormatVersion = 2;

// The version a session file declares, read without migrating or checking
// anything else: what a saver needs to know whether the file it is about to
// replace must first be kept as a pre-migration backup
// (session-format.md, "Versioning"). False when the bytes are not a session
// file at all.
bool file_format_version(std::string_view bytes, std::int64_t& version);

struct WriteInfo {
    std::string written_by;          // "sonora-engine 0.2.0"; diagnostic only
    std::uint64_t save_generation = 0;
};

// Canonical: the same snapshot and WriteInfo always give the same bytes.
// Refuses a snapshot that breaks an invariant rather than write a file that
// would not load.
bool serialize(const Snapshot& s, const WriteInfo& info, std::string& out,
               std::string& error);

struct Loaded {
    Snapshot snapshot;               // revision 0, next_id = max(id) + 1
    std::string written_by;
    std::uint64_t save_generation = 0;
    std::int64_t file_version = 0;   // before migration
    // What the loader changed so that a file an older build wrote still
    // opens, one sentence each with its key path, for the opener to show.
    // Empty for a file this build would have written.
    std::vector<std::string> repairs;
};

struct LoadError {
    std::string message;
    std::string path;    // "tracks[3].playlists[0].regions[12].length", or empty
    int line = 0;        // 0 when the problem has no single place in the text
    int column = 0;
    // "line 12, column 9: tracks[3].name: must be a string"
    std::string text() const;
};

// Strict: unknown, duplicate and missing keys, wrong types, out-of-range
// integers and every invariant are errors with a key path. A file whose
// version is newer than kFormatVersion is refused before anything else is
// read, naming both versions. An older one is migrated in memory first.
bool deserialize(std::string_view bytes, Loaded& out, LoadError& error);

}  // namespace sonora::model
