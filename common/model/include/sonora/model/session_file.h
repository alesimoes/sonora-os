// SPDX-License-Identifier: GPL-3.0-or-later
//
// The native session file (brain/specs/session-format.md): a Snapshot to
// canonical JSON bytes and back. No I/O: serialize() returns bytes and
// deserialize() takes them. Writing them durably is engine/control's job,
// through common/platform (ADR-0009 §5).
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/model/sequence_file.h"

namespace sonora::model {

// The format version this program writes, and the newest it reads.
// 2: plugin inserts on tracks (ADR-0012 §6.2).
// 3: solo safe on tracks (BACKLOG Track 2 item 5).
// 4: the tempo map inside the bounds musical time needs (ADR-0014 §2,
//    brain/specs/midi.md §2.4); a version 3 map is brought inside them.
// 5: MIDI (ADR-0014 §3, midi.md §4): track kinds, MIDI playlists and
//    regions in ticks with their events inline, MIDI routing by port name,
//    midi_sources. A version 4 track is an audio track.
// 6: each MIDI sequence in its own file, "MIDI Sequences/<sha256>.msq",
//    and a region's "data" a reference to it (ADR-0015 §3). A version 5
//    file's inline sequences open as they were and move to files at the
//    first save, which keeps the version 5 file as its backup.
constexpr std::int64_t kFormatVersion = 6;

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
// would not load. The file names each sequence by its hash; `digests`, a
// saver's own, spares hashing again what an earlier save hashed. Writing
// the sequence files themselves, before this file, is the saver's
// (named_sequences(), sequence_file.h).
bool serialize(const Snapshot& s, const WriteInfo& info, std::string& out,
               std::string& error, SequenceDigests* digests = nullptr);

// One value the loader changed so that a file an older build wrote still
// opens. The opener shows each on its own, held until dismissed (ui/qt
// SessionModel::repairs), so the parts are kept apart rather than joined
// into a sentence a status line would elide.
struct Repair {
    std::string path;  // the key changed: "tempo[1].bpm"
    std::string was;   // as the file holds it: "2400.0 bpm"
    std::string now;   // as it opens: "1000 bpm"
    std::string why;   // why it could not open as it was
    // "tempo[1].bpm: 2400.0 bpm -> 1000 bpm (why)", for a log or a test.
    std::string text() const { return path + ": " + was + " -> " + now + " (" + why + ")"; }
};

// Where deserialize() reads the sequence files a session names. The model
// does no I/O: the opener reads them from the session folder, and a Load
// over the control socket from the bundle that carried them.
class SequenceSource {
public:
    virtual ~SequenceSource() = default;
    // `file` is relative to the session folder, "MIDI Sequences/<sha>.msq".
    // False, with `why`, when it is not there or cannot be read.
    virtual bool read(const std::string& file, std::string& bytes, std::string& why) = 0;
};

// A sequence file that was not there or did not verify at open (ADR-0015
// owner decision O2). Its regions opened silent, with their windows, and
// the file's reference kept for the next save.
struct MissingSequence {
    SequenceFileRef ref;
    std::string why;                    // "the file is not there", "its checksum does not match"
    std::vector<std::string> regions;   // key paths: "tracks[2].playlists[0].regions[1]"
    // The MIDI sources those regions came from: a take file to rebuild them
    // from, which the opener offers. Empty when none came from one.
    std::vector<Id> sources;
};

struct Loaded {
    Snapshot snapshot;               // revision 0, next_id = max(id) + 1
    std::string written_by;
    std::uint64_t save_generation = 0;
    std::int64_t file_version = 0;   // before migration
    // What the loader changed so that a file an older build wrote still
    // opens, for the opener to show. Empty for a file this build would
    // have written.
    std::vector<Repair> repairs;
    std::vector<MissingSequence> missing_sequences;
    // The hashes of the sequence files that were read and verified: what a
    // saver may take as already on disk (SequenceFileSet::wrote()).
    std::set<std::string> verified_sequences;
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
//
// Each sequence file is read once from `sequences`, however many regions
// share it, and checked against its reference: size, hash, a strict
// decode, counts. One that is not there or does not verify is a
// MissingSequence, never an error: the session opens. Without `sequences`
// every one is missing.
bool deserialize(std::string_view bytes, Loaded& out, LoadError& error,
                 SequenceSource* sequences = nullptr);

// A session file together with the sequence files it names, in one blob:
// what ModelAction::Load carries (protocol 8), because the engine reads no
// file of the interface's. A sequence whose file is missing travels as its
// reference alone, and opens missing on the other side too.
bool encode_session_bundle(const Snapshot& s, const WriteInfo& info, std::string& out,
                           std::string& error, SequenceDigests* digests = nullptr);
// Takes a bundle or a bare session file, which is a bundle that carries no
// sequence file: every file of format 5 or older is one.
bool decode_session_bundle(std::string_view bytes, Loaded& out, LoadError& error);

}  // namespace sonora::model
