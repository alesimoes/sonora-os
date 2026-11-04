// SPDX-License-Identifier: GPL-3.0-or-later
//
// Plans (ADR-0038): what the mix assistant, or a rule-based pass, proposes.
// A plan is an ordered list of ordinary model Members, each with the words
// that explain it, and nothing else: no audio, no processing of its own. It
// is previewed as a diff, the operator accepts or rejects steps, and the
// accepted ones are applied as ONE Compound through History, so the undo
// path is the one every other edit uses. Nothing here runs on the audio
// thread: it allocates, holds strings and is for the control thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sonora/model/history.h"
#include "sonora/model/model.h"
#include "sonora/model/operations.h"

namespace sonora::model {

struct PlanStep {
    Member op;
    // "Add reverb send on Vox", shown as the step's heading.
    std::string title;
    // The measurement or rule behind the values: "decay 1.9 s: reference
    // estimate 1.8-2.1 s". Display only.
    std::string reason;
    // Indices of EARLIER steps this one cannot apply without (a send needs
    // the aux bus an earlier step adds). Rejecting a step drops its
    // dependents; accepting a dependent never revives a rejected step.
    std::vector<std::size_t> needs;
};

// Something the plan saw and left alone, named so the operator knows
// ("Vox insert 2, third-party plugin: not adjusted").
struct PlanNote {
    std::string subject;
    std::string text;
};

struct Plan {
    // The compound's label, and so the Edit menu's "Undo <label>".
    std::string label;
    // The revision of the snapshot it was proposed on. A plan reads current
    // values (a fader's, to trim it), so it is refused on any other.
    std::uint64_t base_revision = 0;
    // Ids in the members are strictly increasing from the base's next_id,
    // and conditional ids are reserved (ADR-0009, compound operations), so
    // any subset of the steps still applies. Gaps are harmless.
    std::vector<PlanStep> steps;
    std::vector<PlanNote> notes;
};

// Empty, or why the plan is malformed: no steps, more than
// kMaxCompoundMembers, a `needs` that is not an earlier step, a member a
// plan cannot take back (see plan_revertible).
std::string plan_problem(const Plan& plan);

// A plan can take back what History undoes, and the value members whose
// previous value it can read: SetMix, SetSendMix, SetBusMix. The other
// members History does not undo (AddSource, SetSettings, SetInsertParams,
// ...) are refused in a plan, so "one undo reverts it all" is never
// silently false.
bool plan_revertible(const Member& m);

// Where plans come from. Runs on a non-RT thread; `at` is a value, kept
// alive by the caller. False, with `error`, when it has nothing the operator
// could use ("no tempo", "session is empty"): a source never returns an
// empty plan as success.
class PlanSource {
public:
    virtual ~PlanSource() = default;
    virtual bool propose(const Snapshot& at, Plan& out, std::string& error) = 0;
};

// --- selection --------------------------------------------------------------

struct Selection {
    // accepted[i] is the operator's verdict on step i. Shorter than the plan
    // means the missing steps are rejected.
    std::vector<bool> accepted;
    static Selection all(const Plan& plan);
    static Selection none() { return {}; }
};

// The steps that will apply: accepted, with every needed step applied too.
// `dropped` are those accepted but cut off by a rejected dependency.
struct Resolved {
    std::vector<std::size_t> applied;
    std::vector<std::size_t> dropped;
};
Resolved resolve(const Plan& plan, const Selection& sel);

// --- preview ----------------------------------------------------------------

enum class DiffKind { Added, Removed, Changed };

struct DiffLine {
    std::size_t step = 0;   // index into Plan::steps
    DiffKind kind = DiffKind::Changed;
    std::string subject;    // "Track Vox", "Bus Reverb", "Vox send A"
    std::string field;      // "gain_db"; empty for Added and Removed
    std::string before;     // formatted; empty for Added
    std::string after;      // formatted; empty for Removed
};

// The difference between two snapshots in the terms an operator reads:
// tracks, buses, sends, inserts and their mix values.
void diff_snapshots(const Snapshot& before, const Snapshot& after, std::size_t step,
                    std::vector<DiffLine>& out);

struct PlanPreview {
    Resolved resolved;
    std::vector<DiffLine> lines;   // by step, in plan order
    // Exactly what accept_plan() leaves: the preview applies the same
    // members, with the same MemberCheck, to the same snapshot.
    Snapshot result;
};

// Refused, with `error`, on a malformed plan, a stale base_revision, or a
// member the engine's check or the model refuses (named "step 3 (Add reverb
// send): ..."). Nothing is changed.
bool preview(const Snapshot& at, const Plan& plan, const Selection& sel, PlanPreview& out,
             std::string& error, const MemberCheck& check = {});

// --- accept and undo ---------------------------------------------------------

struct PlanReceipt {
    Resolved resolved;
    // The revision History reached; undo_plan() is refused on any other.
    std::uint64_t revision = 0;
    // The compound made an edit-undo step, so History::undo() takes it back.
    bool pushed_step = false;
    // Value members are outside History's undo (ADR-0009 3): the values the
    // plan overwrote, as one compound applied by undo_plan().
    Compound restore;
};

// Applies the resolved steps as one Compound labelled plan.label through
// History::apply: one revision, one update, one undo step. A stale
// base_revision or a refused member leaves the history as it was; an empty
// selection succeeds with an empty receipt and applies nothing.
bool accept_plan(History& history, const Plan& plan, const Selection& sel, PlanReceipt& receipt,
                 std::string& error, const MemberCheck& check = {});

// Takes the plan back: History::undo() for the step, then the restore
// compound for the values. Refused unless the plan is still the latest
// change (history.current().revision == receipt.revision), so it never
// undoes something else.
bool undo_plan(History& history, const PlanReceipt& receipt, std::string& error);

}  // namespace sonora::model
