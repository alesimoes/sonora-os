// SPDX-License-Identifier: GPL-3.0-or-later
//
// MIDI sequences beside the session file (ADR-0015 §3, §6; midi.md §4.4):
// the .msq encoding, the hash that names each file, and what a saver keeps
// in memory to know which files to write and which it may delete.
//
// No I/O, like the rest of common/model: the saver (ui/qt today, the
// engine's saver thread once it has one) reads, writes, syncs and deletes,
// and this says what. Never on the audio thread: it allocates.
#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "sonora/model/model.h"

namespace sonora::model {

// --- the .msq encoding -------------------------------------------------------

// The version this build writes and the only one it reads.
inline constexpr std::uint8_t kSequenceFileVersion = 1;

// Canonical: one sequence has exactly one encoding, so its hash is stable
// across saves, machines and compilers. `s` must be a valid sequence
// (sequence_problem() empty) that is not missing.
std::string encode_sequence(const MidiSequence& s);

// Bounded and non-throwing on any input, as common/midi's SMF reader is:
// refuses a wrong magic or version, a non-minimal or oversized LEB128, a
// count the bytes cannot hold, a tick past 2^53 - 1, trailing bytes, and
// any sequence sequence_problem() would refuse. What it accepts re-encodes
// to the same bytes.
bool decode_sequence(std::string_view bytes, MidiSequence& out, std::string& error);

// The reference a session file holds for `s`: its hash, size and counts.
// `encoded`, when not null, receives the bytes the hash was taken of.
SequenceFileRef sequence_ref(const MidiSequence& s, std::string* encoded = nullptr);

// --- what a save names --------------------------------------------------------

// Each live sequence's reference, so a save hashes only what changed since
// the one before (ADR-0015 §3: "later saves hash what changed"). Keyed by
// identity, but only while the sequence lives: an entry whose sequence was
// freed is never matched again, because its address may be reused. One
// saver owns one; it is not thread-safe.
class SequenceDigests {
public:
    // A missing sequence's reference is the one it was opened with.
    // `encoded` is as sequence_ref()'s, and left empty when the reference
    // was already known.
    const SequenceFileRef& ref(const std::shared_ptr<const MidiSequence>& s,
                               std::string* encoded = nullptr);
    // Drops the entries of sequences no longer alive. ref() calls it when
    // the memo has doubled since the last sweep, so it stays near the
    // number of live sequences.
    void sweep();
    std::size_t size() const { return memo_.size(); }

private:
    struct Entry {
        std::weak_ptr<const MidiSequence> seq;
        SequenceFileRef ref;
    };
    std::unordered_map<const MidiSequence*, Entry> memo_;
    std::size_t swept_at_ = 64;
};

struct NamedSequence {
    std::shared_ptr<const MidiSequence> data;
    SequenceFileRef ref;
    // The .msq bytes, when this call had to encode them to hash them; a
    // saver that must write the file and finds this empty encodes again.
    std::string encoded;
};

// Every distinct sequence `s`'s MIDI regions hold, once each, in track and
// region order: what a session file of `s` names.
std::vector<NamedSequence> named_sequences(const Snapshot& s, SequenceDigests& digests);

// The hashes a session file names, read without loading it: what the
// saver rebuilds its reference sets from at open (ADR-0015 §6). A file of
// a version before 6 holds its MIDI inline and names none. False, with
// `why`, when the bytes do not parse as a session file: its references are
// then unknown.
bool sequence_references(std::string_view session_file, std::set<std::string>& out,
                         std::string& why);

// --- what the saver keeps in memory between saves (ADR-0015 §3, §6) ---------

// What is on disk is a set of hashes, the saver's own: those that verified
// at open, plus each file it renamed into place, minus each it deleted. A
// sequence still held by an undo step or a replica is written again if a
// save names it after its file went.
//
// Which files may go is decided by the kept session files alone (the main
// file, the autosave, each backup, each pre-migration copy): a sequence
// file that none of them names is derived state and is deleted without
// asking. One kept file that could not be read blocks every deletion while
// it is kept, because what it names is unknown.
class SequenceFileSet {
public:
    bool on_disk(const std::string& sha256) const { return written_.count(sha256) != 0; }
    void wrote(const std::string& sha256) { written_.insert(sha256); }
    void deleted(const std::string& sha256) { written_.erase(sha256); }

    // `name` identifies a kept file; a second call for it replaces the first.
    void kept(const std::string& name, std::set<std::string> references);
    void kept_unreadable(const std::string& name);
    // A kept file rotated out or removed.
    void dropped(const std::string& name);

    // The kept files that could not be read, for the operator.
    std::vector<std::string> unreadable() const;
    // Of `present`, the hashes of the .msq files the folder holds, those no
    // kept file names. Empty while a kept file is unreadable.
    std::vector<std::string> deletable(const std::vector<std::string>& present) const;
    void clear();

private:
    std::set<std::string> written_;
    // An unreadable file's entry is absent from refs_ and present here.
    std::map<std::string, std::set<std::string>> refs_;
    std::set<std::string> unreadable_;
};

}  // namespace sonora::model
