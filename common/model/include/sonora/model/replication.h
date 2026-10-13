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
    std::string bytes;              // Op: op_codec bytes; Snapshot: a session file
};

class Document {
public:
    explicit Document(Snapshot initial, std::size_t budget_bytes = History::kDefaultBudget);

    // On success `out` is the update to send to every replica. On failure
    // the document is unchanged and `error` says why, for the reply.
    bool apply(std::string_view op_bytes, ReplicationUpdate& out, std::string& error);
    bool apply(const Operation& op, ReplicationUpdate& out, std::string& error);
    bool undo(ReplicationUpdate& out, std::string& why);
    bool redo(ReplicationUpdate& out, std::string& why);
    // The whole document, for a replica that is new or has lost track.
    bool snapshot(ReplicationUpdate& out, std::string& error) const;

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
