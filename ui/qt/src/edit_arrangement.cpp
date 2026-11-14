// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sections in Memory Locations: Move to / Copy to as ONE Compound (one undo
// step), planned by model::plan_section_edit. Part of EditController
// (edit_controller.h); the SessionModel hooks are edit_arrangement_session.cpp.
#include "edit_arrangement.h"

#include "edit_controller.h"

namespace sonora::ui {

using namespace model;

std::string arrangement_done_message(const std::string& section, SectionMode mode) {
    return std::string(mode == SectionMode::Move ? "Moved" : "Copied") + " \"" + section +
           "\". Left where they were: " + kSectionEditLimits + ".";
}

std::string arrangement_refusal_message(const std::string& reason, SectionMode mode) {
    return reason + " Nothing was " + (mode == SectionMode::Move ? "moved." : "copied.");
}

std::vector<Marker> EditController::arrangement_sections() const {
    return section_markers(snapshot());
}

bool EditController::arrangement_apply(Id section, Frames destination, SectionMode mode,
                                       std::string& message) {
    const Marker* named = nullptr;
    const std::vector<Marker> sections = arrangement_sections();
    for (const Marker& m : sections) {
        if (m.id == section) named = &m;
    }
    if (named == nullptr) {
        message = arrangement_refusal_message("That section is no longer in the session.", mode);
        return false;
    }
    const std::string name = named->name;
    // The plan takes its ids from here; reserved again for every member so
    // nothing else is handed one the plan uses.
    const Id first = doc_.reserve_id();
    SectionPlan plan = plan_section_edit(snapshot(), SectionEdit{section, destination, mode}, first);
    if (!plan.ok()) {
        message = arrangement_refusal_message(plan.refusal, mode);
        return false;
    }
    for (std::size_t i = 0; i < plan.compound.ops.size(); ++i) doc_.reserve_id();
    std::string why;
    if (!submit(std::move(plan.compound), why)) {
        message = arrangement_refusal_message(why, mode);
        return false;
    }
    message = arrangement_done_message(name, mode);
    return true;
}

}  // namespace sonora::ui
