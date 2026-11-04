// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Vocal Balance plan (ADR-0038): analysis in, titled and reasoned steps
// out. Parameters only: each step is an ordinary insert or fader value the
// operator can read, change or undo (ADR-0011 4.1, 4.4).
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "sonora/native/vocal_balance_analysis.h"

namespace sonora::native {

namespace {

using namespace model;

std::string fmt(const char* f, double a, double b = 0.0, double c = 0.0) {
    char buf[160];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

const PluginRef kVocalBalance{PluginFormat::Lv2, "urn:sonora:native:vocal_balance", "Sonora Vocal Balance", "1"};
const PluginRef kDeesser{PluginFormat::Lv2, "urn:sonora:native:deesser", "Sonora De-esser", "1"};
const PluginRef kEq{PluginFormat::Lv2, "urn:sonora:native:eq", "Sonora EQ", "1"};

bool has_plugin(const TrackEdit& t, const PluginRef& p) {
    for (const Insert& i : t.inserts) {
        if (i.plugin.id == p.id) return true;
    }
    return false;
}

}  // namespace

bool VocalBalancePlanSource::propose(const Snapshot& at, Plan& out, std::string& error) {
    const TrackEdit* track = nullptr;
    for (const auto& t : at.edit->tracks) {
        if (t->id == track_) track = t.get();
    }
    if (!track) {
        error = "the track is not in the session";
        return false;
    }
    const VocalAnalysis& a = analysis_;
    Plan plan;
    plan.label = "Vocal Balance on " + track->name;
    plan.base_revision = at.revision;

    Id next = at.next_id;
    int next_slot = 0;
    std::vector<bool> used(kInsertSlots, false);
    for (const Insert& i : track->inserts) {
        if (i.slot < kInsertSlots) used[i.slot] = true;
        next_slot = std::max<int>(next_slot, i.slot + 1);
    }
    auto take_slot = [&](std::uint8_t& slot) {
        for (int s = next_slot; s < kInsertSlots; ++s) {
            if (!used[s]) {
                used[s] = true;
                slot = static_cast<std::uint8_t>(s);
                return true;
            }
        }
        for (int s = 0; s < kInsertSlots; ++s) {
            if (!used[s]) {
                used[s] = true;
                slot = static_cast<std::uint8_t>(s);
                return true;
            }
        }
        return false;
    };

    // Broad zones over the line: Sonora EQ bells.
    {
        std::vector<VocalZone> over;
        for (const VocalZone& z : a.zones) {
            const bool tonal = z.name == "proximity" || z.name == "boxiness" || z.name == "nasal" || z.name == "presence";
            if (tonal && z.excess_db >= 4.0) over.push_back(z);
            if (z.name == "air" && z.excess_db <= -4.0)
                plan.notes.push_back({"Air", fmt("%.1f dB under the spectrum's own tilt above 10 kHz: left alone, "
                                                 "nothing is boosted by this plan", z.excess_db)});
        }
        std::sort(over.begin(), over.end(), [](const VocalZone& x, const VocalZone& y) { return x.excess_db > y.excess_db; });
        if (over.size() > 3) over.resize(3);
        if (!over.empty() && has_plugin(*track, kEq)) {
            plan.notes.push_back({"Sonora EQ", "already on this track: not adjusted"});
        } else if (!over.empty()) {
            AddInsert op;
            op.track = track_;
            op.insert = next++;
            op.plugin = kEq;
            std::string why;
            for (std::size_t i = 0; i < over.size(); ++i) {
                const VocalZone& z = over[i];
                const double f = std::sqrt(z.lo_hz * z.hi_hz);
                const double g = -std::min(6.0, std::max(1.5, 0.5 * z.excess_db));
                const std::string n = std::to_string(i + 1);
                op.params.push_back({"bell" + n + "_on", 1.0f});
                op.params.push_back({"bell" + n + "_freq", static_cast<float>(f)});
                op.params.push_back({"bell" + n + "_gain", static_cast<float>(g)});
                op.params.push_back({"bell" + n + "_q", 1.0f});
                why += (i ? "; " : "") + z.name + fmt(" %.1f dB over the spectrum's tilt: bell at %.0f Hz, %.1f dB", z.excess_db, f, g);
            }
            if (take_slot(op.slot)) {
                plan.steps.push_back({op, "Cut broad zones with Sonora EQ", why, {}});
            } else {
                plan.notes.push_back({"Sonora EQ", "the track has no free insert slot: not added"});
            }
        }
    }

    // The resonance set.
    if (!a.resonances.empty()) {
        if (has_plugin(*track, kVocalBalance)) {
            plan.notes.push_back({"Sonora Vocal Balance", "already on this track: not adjusted"});
        } else {
            AddInsert op;
            op.track = track_;
            op.insert = next++;
            op.plugin = kVocalBalance;
            for (const auto& [k, v] : vocal_balance_params(a)) op.params.push_back({k, v});
            std::string why;
            for (std::size_t i = 0; i < a.resonances.size(); ++i) {
                const VocalResonance& r = a.resonances[i];
                why += (i ? "; " : "") + fmt("%.0f Hz", r.freq_hz) +
                       fmt(" stands %.1f dB over its surroundings in at least 70 %% of the voiced frames "
                           "(cap %.1f dB)", r.excess_db, r.cut_db);
                if (!r.zone.empty()) why += " [" + r.zone + "]";
            }
            if (take_slot(op.slot)) {
                plan.steps.push_back({op,
                                      "Add Sonora Vocal Balance: " + std::to_string(a.resonances.size()) +
                                          (a.resonances.size() == 1 ? " resonance" : " resonances"),
                                      why, {}});
            } else {
                plan.notes.push_back({"Sonora Vocal Balance", "the track has no free insert slot: not added"});
            }
        }
    }

    // Sibilance.
    if (a.sibilance.found && a.sibilance.excess_db >= 4.0) {
        if (has_plugin(*track, kDeesser)) {
            plan.notes.push_back({"Sonora De-esser", "already on this track: not adjusted"});
        } else {
            const double cut = std::min(9.0, std::max(2.0, 0.5 * a.sibilance.excess_db));
            const double ratio = 4.0;
            // The reduction of the median sibilant: (L - T)(1 - 1/R) = cut.
            const double thr = std::min(0.0, std::max(-60.0, a.sibilance.level_db - cut / (1.0 - 1.0 / ratio)));
            AddInsert op;
            op.track = track_;
            op.insert = next++;
            op.plugin = kDeesser;
            op.params = {{"frequency", static_cast<float>(std::min(16000.0, std::max(2000.0, a.sibilance.freq_hz)))},
                         {"threshold", static_cast<float>(thr)},
                         {"ratio", static_cast<float>(ratio)},
                         {"range", 12.0f}};
            const std::string why =
                fmt("%.1f %% of the voiced frames are sibilant, centred at %.0f Hz, ", 100.0 * a.sibilance.fraction,
                    a.sibilance.freq_hz) +
                fmt("%.1f dB over the same band elsewhere: threshold %.1f dB takes about %.1f dB off the median "
                    "sibilant", a.sibilance.excess_db, thr, cut);
            if (take_slot(op.slot)) {
                plan.steps.push_back({op, "Add Sonora De-esser at " + fmt("%.0f Hz", a.sibilance.freq_hz), why, {}});
            } else {
                plan.notes.push_back({"Sonora De-esser", "the track has no free insert slot: not added"});
            }
        }
    }

    // Level: the fader towards a level the plan can state.
    if (const TrackMix* tm = at.mix.find(track_)) {
        double delta = std::min(9.0, std::max(-9.0, target_db_ - a.level_db));
        delta = std::round(delta * 2.0) / 2.0;
        if (std::fabs(delta) >= 1.0) {
            SetMix op{*tm};
            op.mix.gain_db += static_cast<float>(delta);
            plan.steps.push_back({op, fmt("Trim %+.1f dB", delta) + " on " + track->name,
                                  fmt("the voiced frames sit at %.1f dBFS rms (median); the target is %.1f dBFS", a.level_db,
                                      target_db_),
                                  {}});
        }
    }
    if (a.level_spread_db > 12.0) {
        plan.notes.push_back({"Dynamics", fmt("%.1f dB between the loud and quiet voiced frames (10th to 90th "
                                              "percentile): a compressor or fader ride is advised, none is added",
                                              a.level_spread_db)});
    }

    if (plan.steps.empty()) {
        error = "nothing to adjust: no persistent resonance of 3 dB or more, no broad zone 4 dB over its tilt, "
                "no standing-out sibilance, and the level is within 1 dB of the target";
        return false;
    }
    out = std::move(plan);
    return true;
}

}  // namespace sonora::native
