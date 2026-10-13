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

#include "sonora/model/model.h"

namespace sonora::model {

// The format version this program writes, and the newest it reads.
constexpr std::int64_t kFormatVersion = 1;

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
