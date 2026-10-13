// SPDX-License-Identifier: GPL-3.0-or-later
//
// THE MODEL SEAM. Everything the Edit and Mix windows know about the
// session's timeline and mix passes through `TimelineDocument`, the way
// everything they know about the engine passes through EngineAdapter.
//
// The model is common/model's (ADR-0007 M8): its Snapshot, its pure
// apply(), its byte-bounded History. What is still local is *where* the
// document lives: `ModelDocument` holds the History in this process,
// because the operation stream (M7) does not carry edits from the engine
// yet. The swap when it does:
//
//   here                               at M7
//   --------------------------------   --------------------------------------
//   ModelDocument (a History here)     a model::Replica fed by the engine's
//                                      model::Document (replication.h)
//   TimelineDocument::submit(op)       EngineControl::send(the command); the
//                                      engine resolves and applies, and the
//                                      replica changes when the update comes
//                                      back (ReplicationUpdate::Kind::Op)
//   TimelineDocument::undo / redo      the edit.undo / edit.redo commands;
//                                      the replica moves on Kind::Undo/Redo
//   TimelineDocument::reset            Kind::Snapshot: a session was opened
//   TimelineDocument::reserve_id       gone: engine/control allocates ids when
//                                      it resolves a command into an operation
//
// TODO(M7): a ReplicaDocument in place of ModelDocument. #33 put the wire
// in place (ModelRequest / ModelUpdate, FakeEngine's model::Document) but
// not yet what this seam needs to sit on it, so it is not routed there now:
//   - ControlAdapter::poll() drops Inbox::model; the updates must reach a
//     model::Replica, and a reply from a ModelRequest must not be taken by
//     the command path that shares the inbox.
//   - submit() answers at once and EditController reads the snapshot right
//     after it; over a socket the answer is a later reply, so submit()
//     becomes "sent", and a refusal arrives as a CommandReply.
//   - reset() has no ModelAction: File > Open needs the engine to load a
//     session file and send Kind::Snapshot (session-format.md, "Saving").
//   - reserve_id() goes: engine/control allocates ids when it resolves.
// Doing it against FakeEngine alone would leave the demo session (built
// here, with regions and sources) on one side and the engine's empty
// document on the other.
//
// Rules that keep the swap mechanical:
//
//   - The edit controller *resolves* commands into fully resolved
//     operations (edit_controller.cpp, the resolve_* functions). That code
//     moves to engine/control unchanged; nothing else builds an operation.
//   - Nothing here is Qt, and nothing outside SessionModel and main.cpp holds
//     a TimelineDocument.
//   - A drag in progress is interface state (EditController::drag), drawn
//     outside the model, as the spec says; only the drop is an operation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "model_access.h"
#include "sonora/model/history.h"

namespace sonora::ui {

class TimelineDocument {
public:
    virtual ~TimelineDocument() = default;

    virtual const model::Snapshot& snapshot() const = 0;
    // Applies `op`. On refusal the snapshot is unchanged and `why` says what
    // was refused, for the status line.
    virtual bool submit(const model::Operation& op, std::string& why) = 0;
    virtual bool undo(std::string& why) = 0;
    virtual bool redo(std::string& why) = 0;
    virtual bool can_undo() const = 0;
    virtual bool can_redo() const = 0;
    // An id no operation has introduced and none will be given twice, for an
    // operation being resolved: at least the snapshot's next_id, which is
    // what apply() checks.
    virtual model::Id reserve_id() = 0;
    // A different session (one was opened): the snapshot replaced, and a new
    // history, because undo never crosses into another session.
    virtual void reset(model::Snapshot s) = 0;
    uint64_t revision() const { return snapshot().revision; }
};

// The document, held here. Undo is common/model's History: a pointer over
// immutable EditStates, bounded by bytes, with a floor below which steps
// are evicted (ADR-0009 §1). Mix and settings operations change the
// snapshot and push no step.
class ModelDocument final : public TimelineDocument {
public:
    explicit ModelDocument(model::Snapshot initial,
                           std::size_t budget_bytes = model::History::kDefaultBudget);

    const model::Snapshot& snapshot() const override { return history_.current(); }
    bool submit(const model::Operation& op, std::string& why) override;
    bool undo(std::string& why) override;
    bool redo(std::string& why) override;
    bool can_undo() const override { return history_.can_undo(); }
    bool can_redo() const override { return history_.can_redo(); }
    model::Id reserve_id() override;
    void reset(model::Snapshot s) override;

    const model::History& history() const { return history_; }

private:
    std::size_t budget_;
    model::History history_;
    model::Id next_unreserved_ = 0;
};

// An empty session with `tracks` mono tracks named "Audio 1".., each with one
// empty playlist and its input on the capture channel of the same index,
// at 01:00:00:00, 120 bpm 4/4. Built through apply(), so it is valid by the
// same checks a loaded file passes.
model::Snapshot make_empty_session(uint32_t tracks, uint32_t sample_rate);

// The same, with named tracks and sources and regions laid out the way a
// tracking session looks, for the demo and the screenshots. The sources
// name files that do not exist: their audio is synthetic, and the window
// says so.
model::Snapshot make_demo_session(uint32_t tracks, uint32_t sample_rate);

}  // namespace sonora::ui
