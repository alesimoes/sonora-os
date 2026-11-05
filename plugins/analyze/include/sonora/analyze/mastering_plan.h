// SPDX-License-Identifier: GPL-3.0-or-later
//
// The mastering suggestion pass as a PlanSource (ADR-0038, ADR-0046): the
// settings of mastering_rules.h turned into ordinary session operations the
// operator previews, accepts in part or whole, and takes back with one undo.
//
// Steps, in order:
//  0. The console on the target strip: AddInsert at the first free slot
//     after the strip's last insert (the console is the last stage), or
//     ReplaceInsert when the strip already holds one, with every parameter
//     the pass decided. The step's reason lists each finding.
//  1. When the mix is so loud that the console's -12 dB gain cannot reach
//     the target: SetMix on the strip, lowering the fader by the rest. The
//     fader follows the inserts (signal flows pre-fader).
// Inserts that are not the console are left alone and named in the plan's
// notes. The strip is the one the bounce was made of: the model has no
// master insert yet (BACKLOG, "Aux and Master inserts"), so the console
// goes on a stereo strip until it has.
#pragma once

#include "sonora/analyze/mastering_rules.h"
#include "sonora/model/plan.h"

namespace sonora::analyze {

struct MasteringPlanConfig {
    model::Id track = 0;  // the strip that carries the console
    MasteringConfig rules;
};

class MasteringPlanner final : public model::PlanSource {
public:
    MasteringPlanner(MixAnalysis analysis, MasteringPlanConfig config)
        : analysis_(analysis), config_(config) {}

    bool propose(const model::Snapshot& at, model::Plan& out, std::string& error) override;

    // The decisions behind the last successful propose(), for a display that
    // wants each setting on its own line.
    const MasteringSuggestion& suggestion() const { return suggestion_; }

    static constexpr const char* kPluginUri = "urn:sonora:native:mastering";

private:
    MixAnalysis analysis_;
    MasteringPlanConfig config_;
    MasteringSuggestion suggestion_;
};

}  // namespace sonora::analyze
