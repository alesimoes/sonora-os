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

constexpr const char* kOffline = "the engine is not connected; nothing was changed";

}  // namespace

ReplicaDocument::ReplicaDocument(Submit submit, model::Snapshot placeholder)
    : submit_(std::move(submit)), placeholder_(placeholder), shown_(std::move(placeholder)) {}

uint64_t ReplicaDocument::send(api::ModelAction action, std::string bytes) {
    api::ModelRequest r;
    r.action = action;
    r.op = std::move(bytes);
    return submit_(r);
}

bool ReplicaDocument::submit(const model::Operation& op, std::string& why) {
    if (offline_) {
        why = kOffline;
        return false;
    }
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
    if (offline_) {
        why = kOffline;
        return false;
    }
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
    if (offline_) {
        why = kOffline;
        return false;
    }
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

bool ReplicaDocument::reset(model::Snapshot s, std::string& why) {
    if (offline_) {
        why = kOffline;
        return false;
    }
    model::WriteInfo info;
    info.written_by = "sonora-ui";
    std::string bytes, error;
    if (!model::serialize(s, info, bytes, error)) {
        why = "the session could not be sent to the engine: " + error;
        return false;
    }
    Pending p;
    p.kind = Pending::Kind::Load;
    p.load = s;
    p.seq = send(api::ModelAction::Load, std::move(bytes));
    pending_.push_back(std::move(p));
    shown_ = model::History(std::move(s), shown_.budget());
    next_unreserved_ = 0;
    ++session_generation_;
    return true;
}

void ReplicaDocument::flush_plugin_state(model::Id insert, FlushDone done) {
    if (offline_) {
        done(false, kOffline);
        return;
    }
    Flush f;
    f.seq = send(api::ModelAction::FlushPluginState,
                 insert == kNoId ? std::string() : std::to_string(insert));
    f.done = std::move(done);
    flushes_.push_back(std::move(f));
}

bool ReplicaDocument::set_link(bool up) {
    offline_ = !up;
    // Whatever the old connection was asked to flush, it will never answer.
    std::vector<Flush> lost;
    lost.swap(flushes_);
    for (Flush& f : lost) {
        f.done(false, "the connection to the engine was lost before the plugins' state arrived");
    }
    std::size_t dropped = 0;
    bool load_dropped = false;
    for (const Pending& p : pending_) {
        if (p.answered) continue;
        ++dropped;
        load_dropped = load_dropped || p.kind == Pending::Kind::Load;
    }
    if (dropped == 0) return false;
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [](const Pending& p) { return !p.answered; }),
                   pending_.end());
    rebuild();
    if (on_refused) {
        on_refused("the connection to the engine was lost: " + std::to_string(dropped) +
                   (dropped == 1 ? " edit" : " edits") +
                   " it had not confirmed were dropped; the engine's document is shown");
    }
    if (load_dropped && on_load_abandoned) on_load_abandoned();
    return true;
}

bool ReplicaDocument::receive(const std::vector<api::ModelUpdate>& updates,
                              const std::vector<api::CommandReply>& replies) {
    bool changed = false;
    for (const api::ModelUpdate& m : updates) {
        // A whole document: possibly another session (another interface's
        // Open), possibly this one again after a Resync. Either way, an id
        // held across it is not to be trusted.
        if (m.kind == api::ModelUpdateKind::Snapshot) ++session_generation_;
        std::string why;
        if (!replica_.receive(from_wire(m), why)) {
            // Lost track: the whole document again, undo window and all.
            send(api::ModelAction::Resync, {});
        }
        changed = true;
    }
    struct Flushed {
        FlushDone done;
        bool ok;
        std::string why;
    };
    std::vector<Flushed> flushed;
    for (const api::CommandReply& r : replies) {
        const auto f = std::find_if(flushes_.begin(), flushes_.end(),
                                    [&r](const Flush& x) { return x.seq == r.seq; });
        if (f != flushes_.end()) {
            flushed.push_back({std::move(f->done), r.result == DispatchResult::Ok,
                               r.message.empty() && r.result != DispatchResult::Ok
                                   ? "the engine could not collect the plugins' state"
                                   : r.message});
            flushes_.erase(f);
            continue;
        }
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
    // Last, so each sees the updates the flush caused: the engine sent them
    // before its reply, and they were applied above.
    for (Flushed& f : flushed) f.done(f.ok, f.why);
    return changed;
}

void ReplicaDocument::rebuild() {
    model::History h = replica_.synced() ? *replica_.history()
                                         : model::History(placeholder_, shown_.budget());
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
