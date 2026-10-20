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
#include "sonora/model/operations.h"

namespace sonora::model {

// The document an engine starts with when no session file is opened: an
// empty session with these settings and one mono track per name, each with
// one empty playlist, its input the capture channel of the same index.
Snapshot new_session_with_tracks(const SessionSettings& settings,
                                 const std::vector<std::string>& track_names);

struct ReplicationUpdate {
    enum class Kind { Op, Undo, Redo, Snapshot };
    Kind kind = Kind::Op;
    std::uint64_t revision = 0;     // the document's revision after this update
    std::uint64_t undo_floor = 0;   // History::floor() after it
    std::uint64_t next_id = 0;      // Snapshot only
    std::string bytes;              // Op: encode_operation; Snapshot: encode_window
    // Undo and Redo: the label of the step moved over, for "Undo Paste". A
    // replica that holds another label for that step has lost track.
    std::string label;
};

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
    // document from the first update on. While the encoding is larger than
    // `max_bytes` the oldest undo step is evicted here, as the byte budget
    // would; the other replicas follow the raised floor with the next
    // update, and an undo one of them asks for below it is refused, not
    // desynced. Fails only when the current state alone is too large.
    bool snapshot(ReplicationUpdate& out, std::string& error,
                  std::size_t max_bytes = SIZE_MAX);
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

    const Snapshot& current() const { return history_.current(); }
    const History& history() const { return history_; }

private:
    History history_;
};

class Replica {
public:
    // False when the update does not follow on from this replica's state: a
    // revision gap, an undo with nothing to undo, an op that does not apply.
    // The replica is then unsynced, and stays so until a Snapshot arrives;
    // the interface asks for one (ModelAction::Resync).
    bool receive(const ReplicationUpdate& update, std::string& error);

    bool synced() const { return history_.has_value() && synced_; }
    // Only meaningful when synced().
    const Snapshot& current() const { return history_->current(); }
    bool can_undo() const { return synced() && history_->can_undo(); }
    bool can_redo() const { return synced() && history_->can_redo(); }
    const History* history() const { return history_ ? &*history_ : nullptr; }

private:
    std::optional<History> history_;
    bool synced_ = false;
};

}  // namespace sonora::model
