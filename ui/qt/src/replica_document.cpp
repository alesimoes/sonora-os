// SPDX-License-Identifier: GPL-3.0-or-later
#include "replica_document.h"

#include <algorithm>
#include <utility>

#include "sonora/model/op_codec.h"
#include "sonora/model/session_file.h"

namespace sonora::ui {

namespace {

model::ReplicationUpdate from_wire(const api::ModelUpdate& m) {
    model::ReplicationUpdate u;
    switch (m.kind) {
        case api::ModelUpdateKind::Op: u.kind = model::ReplicationUpdate::Kind::Op; break;
        case api::ModelUpdateKind::Undo: u.kind = model::ReplicationUpdate::Kind::Undo; break;
        case api::ModelUpdateKind::Redo: u.kind = model::ReplicationUpdate::Kind::Redo; break;
        case api::ModelUpdateKind::Snapshot:
            u.kind = model::ReplicationUpdate::Kind::Snapshot;
            break;
    }
    u.revision = m.revision;
    u.undo_floor = m.undo_floor;
    u.next_id = m.next_id;
    u.bytes = m.bytes;
    return u;
}

}  // namespace

ReplicaDocument::ReplicaDocument(Submit submit, model::Snapshot placeholder)
    : submit_(std::move(submit)), shown_(std::move(placeholder)) {}

uint64_t ReplicaDocument::send(api::ModelAction action, std::string bytes) {
    api::ModelRequest r;
    r.action = action;
    r.op = std::move(bytes);
    return submit_(r);
}

bool ReplicaDocument::submit(const model::Operation& op, std::string& why) {
    // Checked here first, against what is shown, so a refusal the engine
    // would make anyway reaches the status line in this frame, unsent.
    model::History trial = shown_;
    if (!trial.apply(op, why)) return false;
    std::string bytes;
    if (!model::encode_operation(op, bytes, why)) return false;
    Pending p;
    p.kind = Pending::Kind::Apply;
    p.op = op;
    p.seq = send(api::ModelAction::Apply, std::move(bytes));
    pending_.push_back(std::move(p));
    shown_ = std::move(trial);
    return true;
}

bool ReplicaDocument::undo(std::string& why) {
    if (!shown_.undo()) {
        why = "nothing to undo";
        return false;
    }
    Pending p;
    p.kind = Pending::Kind::Undo;
    p.seq = send(api::ModelAction::Undo, {});
    pending_.push_back(std::move(p));
    return true;
}

bool ReplicaDocument::redo(std::string& why) {
    if (!shown_.redo()) {
        why = "nothing to redo";
        return false;
    }
    Pending p;
    p.kind = Pending::Kind::Redo;
    p.seq = send(api::ModelAction::Redo, {});
    pending_.push_back(std::move(p));
    return true;
}

model::Id ReplicaDocument::reserve_id() {
    next_unreserved_ = std::max(next_unreserved_, shown_.current().next_id);
    return next_unreserved_++;
}

void ReplicaDocument::reset(model::Snapshot s) {
    model::WriteInfo info;
    info.written_by = "sonora-ui";
    std::string bytes, error;
    // A snapshot that came from a file always serializes; one that cannot
    // is shown here alone, and the engine's next update replaces it.
    if (model::serialize(s, info, bytes, error)) {
        Pending p;
        p.kind = Pending::Kind::Load;
        p.load = s;
        p.seq = send(api::ModelAction::Load, std::move(bytes));
        pending_.push_back(std::move(p));
    } else if (on_refused) {
        on_refused("the session could not be sent to the engine: " + error);
    }
    shown_ = model::History(std::move(s), shown_.budget());
    next_unreserved_ = 0;
}

bool ReplicaDocument::receive(const std::vector<api::ModelUpdate>& updates,
                              const std::vector<api::CommandReply>& replies) {
    bool changed = false;
    for (const api::ModelUpdate& m : updates) {
        std::string why;
        if (!replica_.receive(from_wire(m), why)) {
            // Lost track: the whole document again, undo window and all.
            send(api::ModelAction::Resync, {});
        }
        changed = true;
    }
    for (const api::CommandReply& r : replies) {
        const auto it = std::find_if(pending_.begin(), pending_.end(),
                                     [&r](const Pending& p) { return p.seq == r.seq; });
        if (it == pending_.end()) continue;
        if (r.result == DispatchResult::Ok) {
            it->answered = true;
        } else {
            pending_.erase(it);
            changed = true;
            if (on_refused) on_refused(r.message.empty() ? "the engine refused the edit" : r.message);
        }
    }
    // The engine sends a request's reply just before its update, so an
    // accepted request is in the replica once an update has come after its
    // reply. Usually both arrive in one tick.
    if (!updates.empty()) {
        const std::size_t before = pending_.size();
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                      [](const Pending& p) { return p.answered; }),
                       pending_.end());
        changed = changed || pending_.size() != before;
    }
    if (changed) rebuild();
    return changed;
}

void ReplicaDocument::rebuild() {
    if (!replica_.synced()) {
        // Nothing from the engine yet: what is shown stays the placeholder
        // with the requests applied, which is what shown_ already is.
        return;
    }
    model::History h = *replica_.history();
    for (const Pending& p : pending_) {
        std::string why;
        switch (p.kind) {
            case Pending::Kind::Apply: h.apply(p.op, why); break;
            case Pending::Kind::Undo: h.undo(); break;
            case Pending::Kind::Redo: h.redo(); break;
            case Pending::Kind::Load: h = model::History(*p.load, h.budget()); break;
        }
    }
    shown_ = std::move(h);
}

}  // namespace sonora::ui
