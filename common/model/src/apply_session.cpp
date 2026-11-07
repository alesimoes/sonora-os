// SPDX-License-Identifier: GPL-3.0-or-later
//
// apply() for the family SessionOp (ADR-0059):
// sources, the monitor bus, the session settings and markers.
#include "apply_detail.h"

namespace sonora::model::apply_detail {

bool Applier::operator()(const AddSource& op) {
    const Source& s = op.source;
    const std::string why = detail::source_problem(s);
    if (!why.empty()) return fail("source " + str(s.id) + ": " + why);
    if (!fresh(s.id)) return false;
    const std::string taken = path_taken(s.path);
    if (!taken.empty()) return fail("source " + str(s.id) + ": " + taken);
    // Every id below next_id is taken, so a fresh id is the largest and
    // appending keeps the pool sorted.
    auto pool = std::make_shared<SourcePool>(*before_.sources);
    pool->sources.push_back(s);
    after_ = before_;
    after_.sources = std::move(pool);
    after_.next_id = s.id + 1;
    return true;
}

bool Applier::operator()(const SetMonitorBus& op) {
    std::string where;
    const std::string why = detail::bus_problem(op.bus, where);
    if (!why.empty()) return fail("monitor_bus." + where + ": " + why);
    for (const Bus& path : before_.edit->buses) {
        if (path.hardware != kNoHardwareOut &&
            (in_pair(path.hardware, op.bus.outputs[0]) ||
             in_pair(path.hardware, op.bus.outputs[1]))) {
            return fail("outputs " + std::to_string(path.hardware + 1u) + " and " +
                        std::to_string(path.hardware + 2u) + " carry the path \"" + path.name + "\"");
        }
    }
    after_ = before_;
    after_.mix.bus = op.bus;
    return true;
}

bool Applier::operator()(const SetSettings& op) {
    std::string where;
    const std::string why = detail::settings_problem(op.settings, where);
    if (!why.empty()) return fail(where + ": " + why);
    if (op.settings.sample_rate != before_.settings.sample_rate) {
        return fail("the sample rate is fixed when the session is created"
                    " (every source was recorded at " +
                    std::to_string(before_.settings.sample_rate) + " Hz)");
    }
    after_ = before_;
    after_.settings = op.settings;
    return true;
}

bool Applier::operator()(const AddMarker& op) {
    return edit_markers([&](MarkerList& m) {
        const std::string why = detail::marker_problem(op.marker);
        if (!why.empty()) return fail("marker: " + why);
        if (!fresh(op.marker.id)) return false;
        m.markers.push_back(op.marker);
        next_id_ = op.marker.id + 1;
        return true;
    });
}

bool Applier::operator()(const MoveMarker& op) {
    return edit_markers([&](MarkerList& m) {
        for (Marker& k : m.markers) {
            if (k.id != op.marker) continue;
            k.position = op.position;
            const std::string why = detail::marker_problem(k);
            if (!why.empty()) return fail("marker: " + why);
            return true;
        }
        return fail("marker " + str(op.marker) + " does not exist");
    });
}

bool Applier::operator()(const RemoveMarker& op) {
    return edit_markers([&](MarkerList& m) {
        for (auto it = m.markers.begin(); it != m.markers.end(); ++it) {
            if (it->id == op.marker) {
                m.markers.erase(it);
                return true;
            }
        }
        return fail("marker " + str(op.marker) + " does not exist");
    });
}

}  // namespace sonora::model::apply_detail
