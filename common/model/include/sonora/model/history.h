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
#include <string>

#include "sonora/model/model.h"
#include "sonora/model/operations.h"

namespace sonora::model {

class History {
public:
    // timeline-model.md's placeholder; a session setting later.
    static constexpr std::size_t kDefaultBudget = std::size_t{64} << 20;

    explicit History(Snapshot initial, std::size_t budget_bytes = kDefaultBudget);
    static History replica(Snapshot initial);

    const Snapshot& current() const { return current_; }

    // Applies through model::apply(). An undoable operation discards any
    // redo steps, pushes a step and may evict the oldest.
    bool apply(const Operation& op, std::string& error);

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
    std::size_t step_count() const { return steps_.size(); }
    std::size_t undo_depth() const { return pos_; }

private:
    struct Step {
        std::shared_ptr<const EditState> edit;
        std::uint64_t revision = 0;
        std::size_t charge = 0;   // bytes this step alone allocated
    };

    void push(std::shared_ptr<const EditState> edit, std::uint64_t revision);
    void pop_front();

    std::deque<Step> steps_;
    std::size_t pos_ = 0;
    Snapshot current_;
    std::size_t budget_;
    // Sum of every step's charge except the floor's. The floor is the base
    // state the others are counted against; it is not history overhead.
    std::size_t charged_ = 0;
};

// The bytes `state` allocated that `previous` does not share with it: the
// tracks, playlists and marker list it does not have in common, plus its
// own node. What a step is charged.
std::size_t exclusive_bytes(const EditState& state, const EditState& previous);

}  // namespace sonora::model
