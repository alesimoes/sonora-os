// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/model/replication.h"

#include <cstdint>
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
        t.mix.input.assign(1, static_cast<std::uint16_t>(i));
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

Document::Document(Snapshot initial, std::size_t budget_bytes, std::uint64_t midi_max_entries)
    : history_(std::move(initial), budget_bytes, midi_max_entries) {}

bool Document::apply(std::string_view op_bytes, ReplicationUpdate& out, std::string& error) {
    Operation op;
    if (!decode_operation(op_bytes, op, error)) return false;
    return apply(op, out, error);
}

bool Document::apply(const Operation& op, ReplicationUpdate& out, std::string& error,
                     const MemberCheck& check) {
    // Encoded before it is applied, so a value the codec cannot carry (a NaN
    // gain) is refused rather than applied here and lost on the way to the
    // replicas.
    std::string bytes;
    if (!encode_operation(op, bytes, error)) return false;
    if (!history_.apply(op, error, check)) return false;
    out = ReplicationUpdate{};
    out.kind = ReplicationUpdate::Kind::Op;
    out.revision = history_.current().revision;
    out.undo_floor = history_.floor();
    out.bytes = std::move(bytes);
    return true;
}

bool Document::undo(ReplicationUpdate& out, std::string& why) {
    std::string label = history_.undo_label();
    if (!history_.undo()) {
        why = "nothing to undo";
        return false;
    }
    out = ReplicationUpdate{};
    out.kind = ReplicationUpdate::Kind::Undo;
    out.revision = history_.current().revision;
    out.undo_floor = history_.floor();
    out.label = std::move(label);
    return true;
}

bool Document::redo(ReplicationUpdate& out, std::string& why) {
    std::string label = history_.redo_label();
    if (!history_.redo()) {
        why = "nothing to redo";
        return false;
    }
    out = ReplicationUpdate{};
    out.kind = ReplicationUpdate::Kind::Redo;
    out.revision = history_.current().revision;
    out.undo_floor = history_.floor();
    out.label = std::move(label);
    return true;
}

bool encode_snapshot(History history, ReplicationUpdate& out, std::string& error,
                     std::size_t max_window_bytes) {
    // Trimmed on this copy, which shares every state with the document's
    // history, so a snapshot that cannot fit leaves undo as it was.
    WindowParts w;
    for (;;) {
        if (!encode_window(history, w, error)) return false;
        if (w.window.size() <= max_window_bytes) break;
        if (!history.drop_oldest()) {
            error = "the document is " + std::to_string(w.window.size()) +
                    " bytes with no undo step left to drop, over the " +
                    std::to_string(max_window_bytes) + " a snapshot may carry";
            return false;
        }
    }
    out = ReplicationUpdate{};
    out.kind = ReplicationUpdate::Kind::Snapshot;
    out.revision = history.current().revision;
    out.undo_floor = history.floor();
    out.next_id = history.current().next_id;
    out.bytes = std::move(w.window);
    out.parts = std::move(w.parts);
    return true;
}

bool Document::snapshot(ReplicationUpdate& out, std::string& error, std::size_t max_bytes) {
    if (!encode_snapshot(history_, out, error, max_bytes)) return false;
    follow_floor(out.undo_floor);
    return true;
}

void Document::follow_floor(std::uint64_t floor) { history_.evict_below(floor); }

bool Document::load(std::string_view session_file, std::uint32_t sample_rate,
                    ReplicationUpdate& out, std::string& error, std::size_t max_bytes) {
    History previous = history_;
    if (!load(session_file, sample_rate, error)) return false;
    if (!snapshot(out, error, max_bytes)) {
        history_ = std::move(previous);
        return false;
    }
    return true;
}

bool Document::load(std::string_view load, std::uint32_t sample_rate, std::string& error,
                    LoadParts* parts, std::size_t max_window_bytes) {
    Loaded loaded;
    LoadError why;
    bool ok = false;
    if (is_load_in_parts(load)) {
        // A session with no MIDI sends no part.
        LoadParts none(0);
        LoadParts& from = parts != nullptr ? *parts : none;
        std::string_view session_file;
        if (!from.open(load, session_file, error)) {
            error = "the session does not load: " + error;
            return false;
        }
        ok = deserialize(session_file, loaded, why, &from);
    } else if (parts != nullptr && parts->chunks() != 0) {
        error = "the session does not load: the Load after its parts carries no index of them";
        return false;
    } else {
        ok = decode_session_bundle(load, loaded, why);
    }
    if (!ok) {
        error = "the session does not load: " + why.text();
        return false;
    }
    if (sample_rate != 0 && loaded.snapshot.settings.sample_rate != sample_rate) {
        error = "the session is at " + std::to_string(loaded.snapshot.settings.sample_rate) +
                " Hz and this system runs at " + std::to_string(sample_rate) +
                " Hz; it cannot play here until sample-rate conversion exists";
        return false;
    }
    // Refused whole, never opened half-loaded (ADR-0015 §5). An engine
    // setting rather than a session key, so the way in is to raise it.
    const std::uint64_t entries = state_midi_entries(*loaded.snapshot.edit);
    if (entries > history_.midi_max_entries()) {
        error = "the session holds " + std::to_string(entries) +
                " MIDI notes and events, over this engine's midi_max_entries of " +
                std::to_string(history_.midi_max_entries()) + "; raise the setting to open it";
        return false;
    }
    History next(std::move(loaded.snapshot), history_.budget(), history_.midi_max_entries());
    if (max_window_bytes != SIZE_MAX) {
        std::string window;
        if (!encode_window_alone(next, window, error)) return false;
        if (window.size() > max_window_bytes) {
            error = "the session's document is " + std::to_string(window.size()) +
                    " bytes, over the " + std::to_string(max_window_bytes) +
                    " a snapshot may carry, so no interface could be sent it";
            return false;
        }
    }
    history_ = std::move(next);
    return true;
}

bool Replica::attached(std::optional<History> h, const ReplicationUpdate& u,
                       std::string& error) {
    if (h->current().revision != u.revision || h->current().next_id != u.next_id ||
        h->floor() != u.undo_floor) {
        synced_ = false;
        error = "the engine's snapshot disagrees with its own revision, id counter or floor";
        return false;
    }
    history_ = std::move(h);
    synced_ = true;
    return true;
}

bool Replica::receive(const ReplicationUpdate& u, std::string& error) {
    const auto abandon = [&](const std::string& why) {
        decoder_.reset();
        dropping_ = true;
        synced_ = false;
        error = "the engine's snapshot does not load: " + why;
        return false;
    };
    if (u.kind == ReplicationUpdate::Kind::SnapshotHeader) {
        dropping_ = false;
        decoder_.emplace();
        synced_ = false;
        std::string why;
        if (!decoder_->begin(u.part_count, u.part_bytes, max_snapshot_bytes_, why)) {
            return abandon(why);
        }
        return true;
    }
    if (u.kind == ReplicationUpdate::Kind::SnapshotPart) {
        if (dropping_) return true;
        if (!decoder_) return abandon("a part arrived with no header before it");
        std::string why;
        if (!decoder_->part(u.bytes, why)) return abandon(why);
        return true;
    }
    if (u.kind == ReplicationUpdate::Kind::Snapshot) {
        if (dropping_ && !decoder_) {
            dropping_ = false;
            return true;
        }
        std::optional<History> h;
        std::string why;
        bool ok = false;
        if (decoder_ && !u.parts.empty()) {
            why = "a snapshot with parts of its own after a header";
        } else if (decoder_) {
            ok = decoder_->finish(u.bytes, h, why);
        } else {
            WindowParts w;
            w.parts = u.parts;
            w.window = u.bytes;
            WindowDecoder d;
            ok = d.begin(w.parts.size(), w.part_bytes(), max_snapshot_bytes_, why);
            for (std::size_t i = 0; ok && i < w.parts.size(); ++i) ok = d.part(w.parts[i], why);
            ok = ok && d.finish(w.window, h, why);
        }
        decoder_.reset();
        dropping_ = false;
        if (!ok) {
            synced_ = false;
            error = "the engine's snapshot does not load: " + why;
            return false;
        }
        return attached(std::move(h), u, error);
    }
    if (decoder_ || dropping_) {
        // The engine sends nothing to a replica between a Snapshot's header
        // and its window.
        return abandon("an update arrived inside a snapshot");
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
            // Labels are display only, but a different one means a
            // different step: this replica has lost track.
            if (history_->can_undo() && history_->undo_label() != u.label) {
                error = "the engine undid \"" + u.label + "\" and this replica's step is \"" +
                        history_->undo_label() + "\"";
                break;
            }
            ok = history_->undo();
            if (!ok) error = "the engine undid a step this replica does not have";
            break;
        case ReplicationUpdate::Kind::Redo:
            if (history_->can_redo() && history_->redo_label() != u.label) {
                error = "the engine redid \"" + u.label + "\" and this replica's step is \"" +
                        history_->redo_label() + "\"";
                break;
            }
            ok = history_->redo();
            if (!ok) error = "the engine redid a step this replica does not have";
            break;
        case ReplicationUpdate::Kind::Snapshot:
        case ReplicationUpdate::Kind::SnapshotHeader:
        case ReplicationUpdate::Kind::SnapshotPart:
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
