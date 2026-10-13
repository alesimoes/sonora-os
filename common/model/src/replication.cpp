// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/model/replication.h"

#include <utility>

#include "sonora/model/op_codec.h"
#include "sonora/model/session_file.h"

namespace sonora::model {

Snapshot new_session_with_tracks(const SessionSettings& settings,
                                 const std::vector<std::string>& track_names) {
    Snapshot s = new_session(settings);
    for (std::size_t i = 0; i < track_names.size(); ++i) {
        AddTrack t;
        t.track = s.next_id;
        t.playlist = s.next_id + 1;
        t.name = track_names[i];
        t.playlist_name = track_names[i] + ".01";
        t.channels = 1;
        t.mix.input = {static_cast<std::uint16_t>(i)};
        Snapshot next;
        std::string error;
        // Cannot fail for fresh ids and a valid name; if it ever does, the
        // track is left out rather than the session refused.
        if (apply(s, t, next, error)) s = std::move(next);
    }
    // The tracks are where the session starts, not edits to undo.
    s.revision = 0;
    return s;
}

Document::Document(Snapshot initial, std::size_t budget_bytes)
    : history_(std::move(initial), budget_bytes) {}

bool Document::apply(std::string_view op_bytes, ReplicationUpdate& out, std::string& error) {
    Operation op;
    if (!decode_operation(op_bytes, op, error)) return false;
    return apply(op, out, error);
}

bool Document::apply(const Operation& op, ReplicationUpdate& out, std::string& error) {
    // Encoded before it is applied, so a value the codec cannot carry (a NaN
    // gain) is refused rather than applied here and lost on the way to the
    // replicas.
    std::string bytes;
    if (!encode_operation(op, bytes, error)) return false;
    if (!history_.apply(op, error)) return false;
    out = ReplicationUpdate{};
    out.kind = ReplicationUpdate::Kind::Op;
    out.revision = history_.current().revision;
    out.undo_floor = history_.floor();
    out.bytes = std::move(bytes);
    return true;
}

bool Document::undo(ReplicationUpdate& out, std::string& why) {
    if (!history_.undo()) {
        why = "nothing to undo";
        return false;
    }
    out = ReplicationUpdate{};
    out.kind = ReplicationUpdate::Kind::Undo;
    out.revision = history_.current().revision;
    out.undo_floor = history_.floor();
    return true;
}

bool Document::redo(ReplicationUpdate& out, std::string& why) {
    if (!history_.redo()) {
        why = "nothing to redo";
        return false;
    }
    out = ReplicationUpdate{};
    out.kind = ReplicationUpdate::Kind::Redo;
    out.revision = history_.current().revision;
    out.undo_floor = history_.floor();
    return true;
}

bool Document::snapshot(ReplicationUpdate& out, std::string& error) const {
    std::string bytes;
    if (!serialize(history_.current(), WriteInfo{"sonora-engine", 0}, bytes, error)) {
        return false;
    }
    out = ReplicationUpdate{};
    out.kind = ReplicationUpdate::Kind::Snapshot;
    out.revision = history_.current().revision;
    out.undo_floor = history_.floor();
    out.next_id = history_.current().next_id;
    out.bytes = std::move(bytes);
    return true;
}

bool Replica::receive(const ReplicationUpdate& u, std::string& error) {
    if (u.kind == ReplicationUpdate::Kind::Snapshot) {
        Loaded loaded;
        LoadError lerr;
        if (!deserialize(u.bytes, loaded, lerr)) {
            synced_ = false;
            error = "the engine's snapshot does not load: " + lerr.text();
            return false;
        }
        loaded.snapshot.revision = u.revision;
        loaded.snapshot.next_id = u.next_id;
        history_.emplace(History::replica(std::move(loaded.snapshot)));
        synced_ = true;
        return true;
    }
    if (!synced()) {
        error = "waiting for a snapshot";
        return false;
    }
    if (u.revision != history_->current().revision + 1) {
        synced_ = false;
        error = "expected revision " + std::to_string(history_->current().revision + 1) +
                " and the engine sent " + std::to_string(u.revision);
        return false;
    }
    bool ok = false;
    switch (u.kind) {
        case ReplicationUpdate::Kind::Op: {
            Operation op;
            ok = decode_operation(u.bytes, op, error) && history_->apply(op, error);
            break;
        }
        case ReplicationUpdate::Kind::Undo:
            ok = history_->undo();
            if (!ok) error = "the engine undid a step this replica does not have";
            break;
        case ReplicationUpdate::Kind::Redo:
            ok = history_->redo();
            if (!ok) error = "the engine redid a step this replica does not have";
            break;
        case ReplicationUpdate::Kind::Snapshot:
            break;
    }
    if (!ok) {
        synced_ = false;
        return false;
    }
    history_->evict_below(u.undo_floor);
    return true;
}

}  // namespace sonora::model
