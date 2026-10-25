// SPDX-License-Identifier: GPL-3.0-or-later
//
// The two halves of replication (ADR-0009 §2), on top of History.
//
// Document is the engine's half: it applies a resolved operation, undo or
// redo, and says what every replica must be told, revision and undo floor
// included. Replica is an interface's half: it applies what it is told and
// nothing else, and says when it has lost track so the interface can ask
// for the whole document again.
//
// Both ends speak ReplicationUpdate; contracts/api's ModelUpdate carries the
// same fields as opaque bytes, and whoever holds both maps one to the other.
// Never on the audio thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sonora/model/history.h"
#include "sonora/model/model.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/operations.h"
#include "sonora/model/session_file.h"

namespace sonora::model {

// The document an engine starts with when no session file is opened: an
// empty session with these settings and one mono track per name, each with
// one empty playlist, its input the capture channel of the same index.
Snapshot new_session_with_tracks(const SessionSettings& settings,
                                 const std::vector<std::string>& track_names);

// A Snapshot travels as a SnapshotHeader naming its parts, the parts (the
// window's chunk table, WindowParts::parts) and the Snapshot (the window)
// last (ADR-0015 §4). Within a process it may also be one update whose
// `parts` hold the chunk table.
struct ReplicationUpdate {
    enum class Kind { Op, Undo, Redo, Snapshot, SnapshotHeader, SnapshotPart };
    Kind kind = Kind::Op;
    // The document's revision after this update; a Snapshot's, for its
    // header and parts too.
    std::uint64_t revision = 0;
    std::uint64_t undo_floor = 0;   // History::floor() after it
    std::uint64_t next_id = 0;      // Snapshot only
    // Op: encode_operation. Snapshot: WindowParts::window. SnapshotPart: one
    // of WindowParts::parts.
    std::string bytes;
    // Undo and Redo: the label of the step moved over, for "Undo Paste". A
    // replica that holds another label for that step has lost track.
    std::string label;
    // Snapshot, when it is one update: the chunk table, which on the wire
    // travels ahead of it as SnapshotPart updates.
    std::vector<std::string> parts;
    // SnapshotHeader: how many parts follow, and their bytes together.
    std::uint64_t part_count = 0;
    std::uint64_t part_bytes = 0;
};

// What a Snapshot's chunk table may hold for an interface that sets no limit
// of its own (ADR-0015 §4: the replica's cap, an interface setting). The
// 2^24-entry budget's table is about 100 MiB.
inline constexpr std::uint64_t kDefaultMaxSnapshotBytes = std::uint64_t{512} << 20;

// The Snapshot of `history`: a pure function of it, so the engine runs it
// on a worker with a copy taken on the document thread (ADR-0015 §4). While
// the window, the chunk table apart, is larger than `max_window_bytes` the
// copy's oldest undo step is evicted, as the byte budget would; the floor
// it ends with is `out.undo_floor`, which the document then follows
// (Document::follow_floor). Fails only when the current state alone is too
// large, or a chunk is too large for a part.
bool encode_snapshot(History history, ReplicationUpdate& out, std::string& error,
                     std::size_t max_window_bytes = SIZE_MAX);

class Document {
public:
    explicit Document(Snapshot initial, std::size_t budget_bytes = History::kDefaultBudget,
                      std::uint64_t midi_max_entries = History::kDefaultMidiMaxEntries);

    // On success `out` is the update to send to every replica: one for a
    // compound, whatever its size. On failure the document is unchanged and
    // `error` says why, for the reply. `check` is the engine's own refusal,
    // asked before each member with the state the members before it made
    // (operations.h); the engine server and FakeEngine pass one, replicas
    // never do.
    bool apply(std::string_view op_bytes, ReplicationUpdate& out, std::string& error);
    bool apply(const Operation& op, ReplicationUpdate& out, std::string& error,
               const MemberCheck& check = {});
    bool undo(ReplicationUpdate& out, std::string& why);
    bool redo(ReplicationUpdate& out, std::string& why);
    // The whole document with its undo window, for a replica that is new
    // or has lost track, so that it offers the same undo and redo as this
    // document from the first update on. While the window, its chunk table
    // apart, is larger than `max_bytes` the oldest undo step is evicted
    // here (encode_snapshot), as the byte budget
    // would; the other replicas follow the raised floor with the next
    // update, and an undo one of them asks for below it is refused, not
    // desynced. Fails only when the current state alone is too large.
    bool snapshot(ReplicationUpdate& out, std::string& error,
                  std::size_t max_bytes = SIZE_MAX);
    // The engine's half of a Snapshot encoded elsewhere (encode_snapshot):
    // evicts what the encoder's copy evicted.
    void follow_floor(std::uint64_t floor);
    // Another session: `session_file` is a session bundle, or a bare
    // session file (decode_session_bundle(), session_file.h), checked as
    // strictly as a load from disk. The document becomes it, with a new
    // history, because undo never crosses into another session; `out` is
    // the Snapshot every replica is sent. A `sample_rate` other than 0
    // refuses a session at another rate, and one over midi_max_entries is
    // refused by name. On failure the document is unchanged and `error`
    // says why.
    bool load(std::string_view session_file, std::uint32_t sample_rate, ReplicationUpdate& out,
              std::string& error, std::size_t max_bytes = SIZE_MAX);
    // The same without the Snapshot, which the caller encodes. With `parts`,
    // `session_file` is a bare session file whose sequence files came in a
    // Load in parts (session_file.h, LoadParts).
    bool load(std::string_view session_file, std::uint32_t sample_rate, std::string& error,
              SequenceSource* parts = nullptr);

    const Snapshot& current() const { return history_.current(); }
    const History& history() const { return history_; }

private:
    History history_;
};

class Replica {
public:
    // The most a Snapshot's chunk table may announce; over it the attach is
    // refused by name.
    void set_max_snapshot_bytes(std::uint64_t bytes) { max_snapshot_bytes_ = bytes; }
    std::uint64_t max_snapshot_bytes() const { return max_snapshot_bytes_; }

    // False when the update does not follow on from this replica's state: a
    // revision gap, an undo with nothing to undo, an op that does not apply.
    // The replica is then unsynced, and stays so until a Snapshot arrives;
    // the interface asks for one (ModelAction::Resync).
    //
    // A Snapshot in parts: from its header the replica is unsynced, its
    // state untouched, and the new state replaces it only once the last part
    // and the window have decoded. A part that does not decode abandons the
    // attach (false); the rest of that Snapshot is then taken and dropped
    // (true, still unsynced) until another header begins.
    bool receive(const ReplicationUpdate& update, std::string& error);

    bool synced() const { return history_.has_value() && synced_; }
    // Between a Snapshot's header and its window.
    bool attaching() const { return decoder_.has_value(); }
    // Only meaningful when synced().
    const Snapshot& current() const { return history_->current(); }
    bool can_undo() const { return synced() && history_->can_undo(); }
    bool can_redo() const { return synced() && history_->can_redo(); }
    const History* history() const { return history_ ? &*history_ : nullptr; }

private:
    bool attached(std::optional<History> h, const ReplicationUpdate& u, std::string& error);

    std::optional<History> history_;
    bool synced_ = false;
    std::optional<WindowDecoder> decoder_;
    // An attach abandoned: its remaining parts and window are dropped.
    bool dropping_ = false;
    std::uint64_t max_snapshot_bytes_ = kDefaultMaxSnapshotBytes;
};

}  // namespace sonora::model
