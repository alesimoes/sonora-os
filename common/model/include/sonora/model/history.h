// SPDX-License-Identifier: GPL-3.0-or-later
//
// Undo and redo as a pointer over immutable EditStates (ADR-0009 §1).
//
// Step i holds the state after undoable operation i; the first retained step
// is the floor. Undo moves back one step and redo forward. There is no
// inverse operation anywhere, so there is none to get wrong: undo restores
// the thing itself.
//
// The history is bounded by bytes, not steps. Each step is charged what it
// alone allocated (the tracks and playlists it copied, not the ones it shares
// with the step before). While the charge is over the budget the oldest step
// is evicted, but the newest step and the one before it are always kept, so
// even an edit larger than the budget can be undone once.
//
// The engine decides eviction. A replica constructed with replica() never
// evicts on its own count; it follows the floor every history message
// carries, through evict_below(), so both sides offer the same undo steps.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/model/operations.h"

namespace sonora::model {

class History {
public:
    // timeline-model.md's placeholder; a session setting later.
    static constexpr std::size_t kDefaultBudget = std::size_t{64} << 20;
    // ADR-0015 §5: 2^24 notes, events and SysEx messages in the document,
    // the current state and every retained step's own sequences together.
    // The engine's setting `midi_max_entries` overrides it.
    static constexpr std::uint64_t kDefaultMidiMaxEntries = std::uint64_t{1} << 24;

    explicit History(Snapshot initial, std::size_t budget_bytes = kDefaultBudget,
                     std::uint64_t midi_max_entries = kDefaultMidiMaxEntries);
    static History replica(Snapshot initial);

    struct WindowStep {
        std::shared_ptr<const EditState> edit;
        std::uint64_t revision = 0;
        std::string label;
    };
    // A replica's history rebuilt from the engine's window: these steps,
    // oldest first, the pointer at `position`, and `current`, whose edit
    // must be that step's. False, with `error`, when they do not fit
    // together. Charges are recomputed, though a replica never acts on them.
    static bool restore(Snapshot current, std::vector<WindowStep> steps, std::size_t position,
                        std::optional<History>& out, std::string& error);

    const Snapshot& current() const { return current_; }

    // Applies through model::apply(), `check` included. An undoable
    // operation discards any redo steps, pushes a step and may evict the
    // oldest; a compound pushes at most one.
    //
    // MIDI's budget (ADR-0015 §5): history goes before work. Past
    // midi_max_entries the oldest steps are evicted, down to the current
    // state alone if need be, and only an operation whose result alone
    // would still be over is refused, by name.
    bool apply(const Operation& op, std::string& error, const MemberCheck& check = {});

    bool can_undo() const { return pos_ > 0; }
    bool can_redo() const { return pos_ + 1 < steps_.size(); }
    // Each bumps the revision. Sources, mix and settings are not touched:
    // undoing a take leaves its source in the pool.
    bool undo();
    bool redo();

    // The revision of the oldest state undo can still reach. Sent with every
    // operation, Undo and Redo to replicas.
    std::uint64_t floor() const { return steps_.front().revision; }
    // A replica's half of the floor rule: drop every step older than
    // `floor`. Never drops the current state.
    void evict_below(std::uint64_t floor);

    std::size_t charged_bytes() const { return charged_; }
    std::size_t budget() const { return budget_; }
    // Entries counted, as ADR-0015 §5 counts them: the floor's chunks and
    // each later step's own. Counted in entries, never allocator bytes, so
    // every machine and compiler evicts at the same step.
    std::uint64_t midi_entries() const { return entries_; }
    // The current state's alone: what an open or an edit is refused over.
    std::uint64_t current_midi_entries() const;
    std::uint64_t midi_max_entries() const { return max_entries_; }
    std::size_t step_count() const { return steps_.size(); }
    std::size_t undo_depth() const { return pos_; }
    // Step `i`, oldest first, for whoever sends the window to a replica.
    const std::shared_ptr<const EditState>& step_edit(std::size_t i) const {
        return steps_[i].edit;
    }
    std::uint64_t step_revision(std::size_t i) const { return steps_[i].revision; }
    const std::string& step_label(std::size_t i) const { return steps_[i].label; }

    // What undo and redo would move over, for "Undo Paste" and "Redo
    // Record": a compound's label, or else display_name() of the operation
    // that made the step. Empty when there is nothing to undo or redo.
    const std::string& undo_label() const;
    const std::string& redo_label() const;

    // The engine's half of shrinking the window: evicts the oldest step if
    // undo can still reach a later one. False when nothing can go.
    bool drop_oldest();

private:
    struct Step {
        std::shared_ptr<const EditState> edit;
        std::uint64_t revision = 0;
        std::size_t charge = 0;   // bytes this step alone allocated
        // Display only. Empty for the state at open.
        std::string label;
        // MIDI entries in the chunks this step holds and the one before it
        // does not; the floor's, every entry it holds.
        std::uint64_t entries = 0;
        // Every entry in the chunks this step holds: what an edit is refused
        // over, kept so it is never counted from scratch.
        std::uint64_t total = 0;
    };
    struct Cost {
        std::size_t charge = 0;
        std::uint64_t entries = 0;
        std::uint64_t total = 0;
    };
    // `next`'s charge, own entries and total, against the step before it.
    static Cost cost(const EditState& next, const Step& previous);

    void push(std::shared_ptr<const EditState> edit, std::uint64_t revision, std::string label,
              const Cost& c);
    void pop_front();
    // An insert's values (MixState::inserts) are kept while a step in the
    // window holds the insert, so undo and redo bring it back as it was.
    // Called whenever steps leave the window, on the engine and on a
    // replica alike, so both drop the same entries at the same revision.
    void prune_insert_values();

    std::deque<Step> steps_;
    std::size_t pos_ = 0;
    Snapshot current_;
    std::size_t budget_;
    // Sum of every step's charge except the floor's. The floor is the base
    // state the others are counted against; it is not history overhead.
    std::size_t charged_ = 0;
    std::uint64_t max_entries_;
    std::uint64_t entries_ = 0;   // sum of every step's `entries`
};

// The bytes `state` allocated that `previous` does not share with it: the
// tracks, playlists and marker list it does not have in common, plus its
// own node. What a step is charged.
std::size_t exclusive_bytes(const EditState& state, const EditState& previous);

// MIDI entries in the distinct chunks `state` holds, and in those it holds
// that `previous` does not (ADR-0015 §5: counted per chunk, so a chunk two
// sequences or two steps share is counted once).
std::uint64_t state_midi_entries(const EditState& state);
std::uint64_t exclusive_midi_entries(const EditState& state, const EditState& previous);

}  // namespace sonora::model
