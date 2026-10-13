// SPDX-License-Identifier: GPL-3.0-or-later
//
// THE MODEL SEAM. Everything the Edit and Mix windows know about the
// session's timeline and mix passes through `TimelineDocument`, the way
// everything they know about the engine passes through EngineAdapter.
//
// What it stands for (timeline-model.md, "What the UI replica gets"): the
// ui/client replica of the engine's Document. Today the replica *is* the
// document -- `LocalDocument` applies operations itself, keeps the undo
// pointer and allocates ids -- because neither common/model (M8) nor the
// operation stream (M7) exists on main. The swap:
//
//   here                              when common/model and M7 land
//   -------------------------------   ---------------------------------------
//   ui::model::*  (timeline_model.h)  sonora::model::*  (common/model)
//   ui::model::apply                  sonora::model::apply, same signature
//   TimelineDocument::snapshot        the replica's Snapshot
//   TimelineDocument::submit(op)      EngineControl::send(the command); the
//                                     engine resolves and applies, and the
//                                     replica changes when the op comes back
//   TimelineDocument::undo / redo     the edit.undo / edit.redo commands; the
//                                     replica moves its pointer on Undo{} /
//                                     Redo{} from the engine
//   TimelineDocument::reserve_id      gone: engine/control allocates ids when
//                                     it resolves a command into an operation
//   TimelineDocument::revision        Snapshot::revision, checked for gaps
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

#include <cstdint>
#include <string>
#include <vector>

#include "timeline_model.h"

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
    // An id nothing in the session uses, for an operation being resolved.
    virtual model::Id reserve_id() = 0;
    uint64_t revision() const { return snapshot().revision; }
};

// The stand-in. Undo is a pointer into a list of EditStates (spec, "Undo is
// a pointer"), so there is no inverse operation to get wrong. Mix operations
// change the current snapshot and push no step. Bounded by step count, not
// bytes: the byte budget and its eviction are the engine's (ADR-0009 §1),
// and a stand-in that imitated them would be a second implementation.
class LocalDocument final : public TimelineDocument {
public:
    explicit LocalDocument(model::Snapshot initial, std::size_t max_steps = 1000);

    const model::Snapshot& snapshot() const override { return current_; }
    bool submit(const model::Operation& op, std::string& why) override;
    bool undo(std::string& why) override;
    bool redo(std::string& why) override;
    bool can_undo() const override { return at_ > 0; }
    bool can_redo() const override { return at_ + 1 < history_.size(); }
    model::Id reserve_id() override { return ++last_id_; }

    std::size_t steps() const { return history_.size(); }

private:
    model::Snapshot current_;
    std::vector<std::shared_ptr<const model::EditState>> history_;
    std::size_t at_ = 0;
    std::size_t max_steps_;
    model::Id last_id_ = 0;
};

// An empty session with `tracks` mono tracks named "Audio 1".., each with one
// empty playlist, the default settings (48 kHz, 01:00:00:00, 120 bpm 4/4).
model::Snapshot make_empty_session(uint32_t tracks, uint32_t sample_rate);

// The same, with named tracks and synthetic sources and regions laid out the
// way a tracking session looks, for the demo and the screenshots. Every
// source is synthetic; the window says so.
model::Snapshot make_demo_session(uint32_t tracks, uint32_t sample_rate);

}  // namespace sonora::ui
