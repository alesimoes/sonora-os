// SPDX-License-Identifier: GPL-3.0-or-later
//
// THE MODEL SEAM. Everything the Edit and Mix windows know about the
// session's timeline and mix passes through `TimelineDocument`, the way
// everything they know about the engine passes through EngineAdapter.
//
// The model is common/model's (ADR-0007 M8): its Snapshot, its pure
// apply(), its byte-bounded History. Since M7 the document sonora-ui edits
// is the engine's: `ReplicaDocument` (replica_document.h) keeps a
// model::Replica fed by the engine's updates and shows it with the requests
// in flight applied on top. `ModelDocument` holds a History in this
// process, for the tests that need no engine.
//
//   TimelineDocument          ReplicaDocument, on the engine
//   ------------------------  ---------------------------------------------
//   submit(op)                ModelAction::Apply with the resolved op;
//                             shown at once, taken back if refused
//   undo / redo               ModelAction::Undo / Redo
//   reset (File > Open)       ModelAction::Load with the session file's
//                             bytes; every replica gets a Snapshot
//   reserve_id                from what is shown; a clash with another
//                             interface's edit is refused by the engine
//
// Still open: the edit controller resolves operations here, in the
// interface. Moving the resolve_* functions to engine/control, so a key is
// a command the engine resolves and allocates ids for, is the next step.
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
    // history, because undo never crosses into another session. Refused,
    // with `why`, when the document's owner cannot be reached.
    virtual bool reset(model::Snapshot s, std::string& why) = 0;
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
    bool reset(model::Snapshot s, std::string& why) override;

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
