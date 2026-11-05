// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/analyze/mastering_plan.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace sonora::analyze {

namespace {

constexpr const char* kNativePrefix = "urn:sonora:native:";

std::string db_text(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", v);
    return buf;
}

}  // namespace

bool MasteringPlanner::propose(const model::Snapshot& at, model::Plan& out, std::string& error) {
    using namespace model;
    const TrackEdit* track = nullptr;
    for (const auto& t : at.edit->tracks) {
        if (t->id == config_.track) track = t.get();
    }
    if (track == nullptr) {
        error = "the strip for the mastering console is not in the session";
        return false;
    }
    if (track->kind != TrackKind::Audio || track->channels == 0 || track->channels > 2) {
        error = "the mastering console goes on a mono or stereo audio strip";
        return false;
    }
    MasteringSuggestion s;
    if (!suggest_mastering(analysis_, config_.rules, s, error)) return false;

    // The console is the last stage of the strip: the slot after the last
    // insert, or the one it already occupies.
    const Insert* existing = nullptr;
    int last_slot = -1;
    Plan plan;
    for (const Insert& ins : track->inserts) {
        last_slot = std::max<int>(last_slot, ins.slot);
        if (ins.plugin.id == kPluginUri) {
            existing = &ins;
        } else if (ins.plugin.id.compare(0, std::char_traits<char>::length(kNativePrefix), kNativePrefix) == 0) {
            plan.notes.push_back({"Insert " + std::to_string(ins.slot + 1) + ", " + ins.plugin.name,
                                  "a Sonora processor already on the strip: not adjusted"});
        } else {
            plan.notes.push_back({"Insert " + std::to_string(ins.slot + 1) + ", " + ins.plugin.name,
                                  "third-party plugin: not adjusted"});
        }
    }
    if (existing == nullptr && last_slot + 1 >= static_cast<int>(kInsertSlots)) {
        error = "the strip has no free insert slot for the console";
        return false;
    }

    PluginRef ref;
    ref.format = PluginFormat::Lv2;
    ref.id = kPluginUri;
    ref.name = native::mastering_descriptor().name;
    std::vector<ParamValue> params;
    const native::Descriptor& d = native::mastering_descriptor();
    for (std::uint32_t i = 1; i < d.param_count; ++i) params.push_back({d.params[i].id, s.values[i]});

    std::string reasons;
    for (const Finding& f : s.findings) {
        if (!reasons.empty()) reasons += '\n';
        reasons += f.title + ": " + f.reason;
    }
    if (reasons.empty()) reasons = "No setting needed to change: the mix is within the rules' tolerances.";

    Id next = at.next_id;
    plan.label = "Mastering Plan";
    plan.base_revision = at.revision;
    if (existing != nullptr) {
        ReplaceInsert op;
        op.insert = existing->id;
        op.replacement = next++;
        op.plugin = ref;
        op.params = std::move(params);
        plan.steps.push_back({op, "Update Mastering Console on " + track->name, reasons, {}});
    } else {
        AddInsert op;
        op.track = track->id;
        op.insert = next++;
        op.slot = static_cast<std::uint8_t>(last_slot + 1);
        op.plugin = ref;
        op.params = std::move(params);
        plan.steps.push_back({op, "Add Mastering Console to " + track->name, reasons, {}});
    }
    if (s.fader_trim_db < 0.0) {
        const TrackMix* mix = at.mix.find(track->id);
        if (mix != nullptr) {
            SetMix op{*mix};
            op.mix.gain_db = std::max(kMinGainDb, op.mix.gain_db + static_cast<float>(s.fader_trim_db));
            plan.steps.push_back({op, "Lower " + track->name + " fader by " + db_text(-s.fader_trim_db) + " dB",
                                  s.fader_reason, {}});
        }
    }
    for (const std::string& o : s.observations) plan.notes.push_back({"Measured", o});

    const std::string problem = plan_problem(plan);
    if (!problem.empty()) {
        error = problem;
        return false;
    }
    suggestion_ = std::move(s);
    out = std::move(plan);
    return true;
}

}  // namespace sonora::analyze
