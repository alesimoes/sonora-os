// SPDX-License-Identifier: GPL-3.0-or-later
//
// The contract's stub (ADR-0123): every import is refused until the planner
// slice lands, so a caller built against the header fails loudly instead of
// importing nothing and reporting success.
#include "sonora/session_files/session_import.h"

namespace sonora::session_files {

bool plan_session_import(const model::Snapshot&, const model::Snapshot&,
                         const SessionImportRequest&, ImportPlan& plan,
                         ImportRefusal& refusal) {
    plan = ImportPlan{};
    refusal = ImportRefusal{};
    refusal.reason = ImportRefusal::Reason::InvalidRequest;
    refusal.message = "session import: not implemented";
    return false;
}

std::vector<ImportableTrack> list_importable_tracks(const model::Snapshot&) {
    return {};
}

}  // namespace sonora::session_files
