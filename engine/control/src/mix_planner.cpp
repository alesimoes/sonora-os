// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/mix_planner.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>

#include "sonora/dsp/loudness.h"
#include "sonora/wav_reader.h"

namespace sonora {

namespace {

using namespace model;

constexpr std::uint32_t kBlock = 4096;

std::string num(double v, const char* fmt = "%.1f") {
    char buf[48];
    std::snprintf(buf, sizeof buf, fmt, v);
    return buf;
}

double to_db(double linear) { return linear > 1e-10 ? 20.0 * std::log10(linear) : -200.0; }

const TrackMix* mix_of(const Snapshot& s, Id track) { return s.mix.find(track); }

bool is_audio(const TrackEdit& t) { return t.kind == TrackKind::Audio && t.channels > 0; }

// --- analysis ------------------------------------------------------------------

struct Totals {
    double frames_done = 0;
    double frames_all = 0;
};

const Playlist* active_playlist(const TrackEdit& t) {
    for (const auto& p : t.playlists) {
        if (p->id == t.active_playlist) return p.get();
    }
    return nullptr;
}

// False only when cancelled.
bool analyse_track(const Snapshot& snap, const std::string& dir, const TrackEdit& t,
                   const AnalysisHooks& hooks, Totals& totals, TrackAnalysis& out) {
    out.track = t.id;
    const Playlist* pl = active_playlist(t);
    if (pl == nullptr) return true;

    dsp::LoudnessMeter meter;
    if (!meter.prepare(snap.settings.sample_rate, t.channels, nullptr, true)) {
        out.problem = "the loudness meter does not take " + std::to_string(t.channels) +
                      " channels";
        return true;
    }
    std::map<Id, std::unique_ptr<WavReader>> readers;
    std::vector<std::vector<float>> planar(t.channels, std::vector<float>(kBlock));
    std::vector<const float*> ptrs(t.channels);
    std::vector<float> raw;
    double sumsq = 0.0;
    double peak = 0.0;
    Frames measured = 0;

    for (const Region& r : pl->regions) {
        if (r.mute) continue;
        if (r.channels.size() != t.channels) {
            out.problem = "region \"" + r.name + "\" does not match the track's channels";
            return true;
        }
        for (const SourceRef& ref : r.channels) {
            if (readers.count(ref.source) != 0) continue;
            const Source* src = snap.sources->find(ref.source);
            if (src == nullptr) {
                out.problem = "region \"" + r.name + "\": its source is not in the pool";
                return true;
            }
            auto rd = std::make_unique<WavReader>();
            std::string why;
            if (!rd->open(dir + "/" + src->path, why)) {
                out.problem = src->path + ": " + why;
                return true;
            }
            if (rd->sample_rate() != snap.settings.sample_rate || ref.channel >= rd->channels()) {
                out.problem = src->path + ": not at the session rate or has no such channel";
                return true;
            }
            readers[ref.source] = std::move(rd);
        }
        const float gain = std::pow(10.0f, r.gain_db / 20.0f);
        for (Frames done = 0; done < r.length;) {
            if (hooks.cancel != nullptr && hooks.cancel->load(std::memory_order_relaxed))
                return false;
            const Frames want = std::min<Frames>(kBlock, r.length - done);
            Frames got_all = want;
            for (std::uint16_t c = 0; c < t.channels; ++c) {
                WavReader& rd = *readers[r.channels[c].source];
                raw.resize(static_cast<std::size_t>(want) * rd.channels());
                const Frames got = rd.read(r.source_offset + done, want, raw.data());
                if (got < 0) {
                    out.problem = "read error in a source of region \"" + r.name + "\"";
                    return true;
                }
                got_all = std::min(got_all, got);
                for (Frames i = 0; i < got; ++i) {
                    planar[c][static_cast<std::size_t>(i)] =
                        raw[static_cast<std::size_t>(i) * rd.channels() + r.channels[c].channel] *
                        gain;
                }
            }
            if (got_all <= 0) {
                out.problem = "region \"" + r.name + "\" is longer than its source";
                return true;
            }
            for (std::uint16_t c = 0; c < t.channels; ++c) {
                ptrs[c] = planar[c].data();
                for (Frames i = 0; i < got_all; ++i) {
                    const double v = planar[c][static_cast<std::size_t>(i)];
                    sumsq += v * v;
                    peak = std::max(peak, std::fabs(v));
                }
            }
            meter.process(ptrs.data(), static_cast<std::uint32_t>(got_all));
            measured += got_all;
            done += got_all;
            totals.frames_done += static_cast<double>(got_all);
            if (hooks.progress && totals.frames_all > 0)
                hooks.progress(std::min(1.0, totals.frames_done / totals.frames_all));
            if (got_all < want) break;
        }
    }
    out.frames = measured;
    if (measured > 0) {
        out.peak_dbfs = to_db(peak);
        out.rms_dbfs = 10.0 * std::log10(std::max(1e-20, sumsq / (static_cast<double>(measured) * t.channels)));
        out.lufs = meter.integrated_lufs();
        out.true_peak_dbtp = meter.true_peak_dbtp();
    }
    return true;
}

// --- names ---------------------------------------------------------------------

std::vector<std::string> tokens(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) != 0) {
            cur.push_back(static_cast<char>(std::tolower(c)));
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

struct Keyword {
    const char* word;
    bool prefix;   // longer words that start with it count ("toms", "kicks")
    Role role;
};

// Bass before Guitars, so "Bass Guitar" is a bass. Short abbreviations match
// whole tokens only: "oh" must not claim "ohm".
const Keyword kKeywords[] = {
    {"kick", true, Role::Drums},    {"snare", true, Role::Drums},   {"tom", true, Role::Drums},
    {"hat", true, Role::Drums},     {"hihat", true, Role::Drums},   {"hh", false, Role::Drums},
    {"oh", false, Role::Drums},     {"overhead", true, Role::Drums}, {"drum", true, Role::Drums},
    {"cymbal", true, Role::Drums},  {"ride", false, Role::Drums},  {"crash", false, Role::Drums},
    {"perc", true, Role::Drums},    {"bd", false, Role::Drums},     {"sn", false, Role::Drums},
    {"bass", true, Role::Bass},     {"sub", false, Role::Bass},     {"bs", false, Role::Bass},
    {"gtr", true, Role::Guitars},   {"guitar", true, Role::Guitars}, {"acoustic", true, Role::Guitars},
    {"electric", true, Role::Guitars}, {"strat", true, Role::Guitars},
    {"piano", true, Role::Keys},    {"key", true, Role::Keys},      {"synth", true, Role::Keys},
    {"organ", true, Role::Keys},    {"rhodes", true, Role::Keys},   {"wurli", true, Role::Keys},
    {"pad", true, Role::Keys},
    {"vox", true, Role::Vocals},    {"vocal", true, Role::Vocals},  {"voice", true, Role::Vocals},
    {"bv", false, Role::Vocals},    {"bgv", false, Role::Vocals},   {"choir", true, Role::Vocals},
    {"harmony", true, Role::Vocals}, {"backing", true, Role::Vocals},
};

bool match_name(const std::string& name, Role& role, std::string& word) {
    const std::vector<std::string> toks = tokens(name);
    // Roles in table order, tokens in name order within a role.
    for (const Keyword& k : kKeywords) {
        for (const std::string& t : toks) {
            const bool hit = t == k.word || (k.prefix && t.rfind(k.word, 0) == 0);
            if (hit) {
                role = k.role;
                word = k.word;
                return true;
            }
        }
    }
    return false;
}

RoleGuess guess_by_name_or_plugin(const TrackEdit& t) {
    RoleGuess g;
    std::string word;
    if (match_name(t.name, g.role, word)) {
        g.why = "name \"" + t.name + "\" matches \"" + word + "\"";
        return g;
    }
    for (const Insert& in : t.inserts) {
        const std::string id = in.plugin.id;
        if (id.find("deesser") != std::string::npos) {
            g = {Role::Vocals, "insert \"" + in.plugin.name + "\" is a de-esser"};
            return g;
        }
        if (id.find("ampcab") != std::string::npos) {
            g = {Role::Guitars, "insert \"" + in.plugin.name + "\" is an amp and cabinet"};
            return g;
        }
        if (match_name(in.plugin.name, g.role, word)) {
            g.why = "insert \"" + in.plugin.name + "\" matches \"" + word + "\"";
            return g;
        }
    }
    return {};
}

// New tracks take the palette in turn (auto_colour), so a shared colour means
// something only when the operator moved it off the one its place gets.
bool colour_chosen(const EditState& edit, const TrackEdit& t) {
    for (std::size_t i = 0; i < edit.tracks.size(); ++i) {
        if (edit.tracks[i]->id == t.id) return t.colour != auto_colour(i);
    }
    return false;
}

// --- plan assembly ---------------------------------------------------------------

Id next_free_id(const Snapshot& at, const Plan& plan) {
    Id next = at.next_id;
    for (const PlanStep& s : plan.steps) {
        if (const auto* b = std::get_if<AddBus>(&s.op)) next = std::max(next, b->bus + 1);
        if (const auto* a = std::get_if<AddSend>(&s.op)) next = std::max(next, a->send + 1);
        if (const auto* i = std::get_if<AddInsert>(&s.op)) next = std::max(next, i->insert + 1);
    }
    return next;
}

const Bus* find_bus_named(const EditState& e, const std::string& name) {
    for (const Bus& b : e.buses) {
        if (b.name == name) return &b;
    }
    return nullptr;
}

bool has_send_slot(const TrackEdit& t, std::uint8_t& slot) {
    for (std::uint8_t s = 0; s < kSendSlots; ++s) {
        const bool taken = std::any_of(t.sends.begin(), t.sends.end(),
                                       [&](const Send& x) { return x.slot == s; });
        if (!taken) {
            slot = s;
            return true;
        }
    }
    return false;
}

double round_half(double v) { return std::round(v * 2.0) / 2.0; }
double floor_half(double v) { return std::floor(v * 2.0) / 2.0; }

ParamValue pv(const char* key, float value) { return ParamValue{key, value}; }

std::string describe(const ProcessorSetting& p) {
    std::string s = p.name + " (" + p.uri + "):";
    for (const ParamValue& v : p.params) s += " " + v.key + " " + num(v.value, "%g") + ";";
    return s;
}

}  // namespace

const TrackAnalysis* MixAnalysis::find(Id track) const {
    for (const TrackAnalysis& t : tracks) {
        if (t.track == track) return &t;
    }
    return nullptr;
}

bool analyse_session(const Snapshot& snapshot, const std::string& session_dir,
                     const AnalysisHooks& hooks, MixAnalysis& out, std::string& error) {
    out.tracks.clear();
    Totals totals;
    for (const auto& t : snapshot.edit->tracks) {
        if (!is_audio(*t)) continue;
        if (const Playlist* pl = active_playlist(*t)) {
            for (const Region& r : pl->regions) {
                if (!r.mute) totals.frames_all += static_cast<double>(r.length);
            }
        }
    }
    for (const auto& t : snapshot.edit->tracks) {
        if (!is_audio(*t)) continue;
        TrackAnalysis a;
        if (!analyse_track(snapshot, session_dir, *t, hooks, totals, a)) {
            error = "the analysis was cancelled";
            return false;
        }
        out.tracks.push_back(std::move(a));
    }
    return true;
}

const char* role_name(Role role) {
    switch (role) {
        case Role::Drums: return "Drums";
        case Role::Bass: return "Bass";
        case Role::Guitars: return "Guitars";
        case Role::Keys: return "Keys";
        case Role::Vocals: return "Vocals";
        case Role::Unknown: break;
    }
    return "Unknown";
}

RoleGuess classify_track(const EditState& edit, const TrackEdit& track) {
    RoleGuess g = guess_by_name_or_plugin(track);
    if (g.role != Role::Unknown || !colour_chosen(edit, track)) return g;
    // A colour is the operator's grouping: borrow it only when every other
    // classified track of that colour agrees.
    Role agreed = Role::Unknown;
    std::string by;
    for (const auto& o : edit.tracks) {
        if (o->id == track.id || o->colour != track.colour || !colour_chosen(edit, *o)) continue;
        const RoleGuess og = guess_by_name_or_plugin(*o);
        if (og.role == Role::Unknown) continue;
        if (agreed != Role::Unknown && agreed != og.role) return {};
        if (agreed == Role::Unknown) by = o->name;
        agreed = og.role;
    }
    if (agreed == Role::Unknown) return {};
    return {agreed, "same colour as \"" + by + "\" (" + role_name(agreed) + ")"};
}

// --- gain staging ------------------------------------------------------------------

bool GainStagingPlanner::propose(const Snapshot& at, Plan& out, std::string& error) {
    struct Cand {
        const TrackEdit* track;
        const TrackAnalysis* a;
        TrackMix mix;
        double delta = 0.0;
        std::string limit;
    };
    Plan plan;
    plan.label = "Gain Staging";
    plan.base_revision = at.revision;
    std::vector<Cand> cands;
    for (const auto& t : at.edit->tracks) {
        if (!is_audio(*t)) continue;
        const TrackMix* m = mix_of(at, t->id);
        const TrackAnalysis* a = analysis_.find(t->id);
        if (m == nullptr || a == nullptr) continue;
        if (!a->problem.empty()) {
            plan.notes.push_back({"Track " + t->name, "not measured: " + a->problem});
        } else if (a->silent()) {
            plan.notes.push_back({"Track " + t->name, "silent: not adjusted"});
        } else if (m->mute) {
            plan.notes.push_back({"Track " + t->name, "muted: not adjusted"});
        } else {
            cands.push_back({t.get(), a, *m, 0.0, {}});
        }
    }
    if (cands.empty()) {
        error = "no track has audio to measure";
        return false;
    }

    for (Cand& c : cands) {
        const double post_lufs = c.a->lufs + c.mix.gain_db;
        const double post_tp = c.a->true_peak_dbtp + c.mix.gain_db;
        c.delta = options_.target_lufs - post_lufs;
        // The ceiling limits a boost; a cut is never held back by it.
        if (post_tp + c.delta > options_.ceiling_dbtp) {
            c.delta = std::min(c.delta, options_.ceiling_dbtp - post_tp);
            c.limit = "limited by the " + num(options_.ceiling_dbtp) + " dBTP peak ceiling";
        }
        // Rounding a capped boost to the nearest half dB could step over the cap.
        c.delta = c.limit.empty() ? round_half(c.delta) : floor_half(c.delta);
    }

    // The master: powers of uncorrelated peaks add. Over the headroom, every
    // track comes down together, which keeps the balance the trims made.
    double power = 0.0;
    for (const Cand& c : cands)
        power += std::pow(10.0, (c.a->true_peak_dbtp + c.mix.gain_db + c.delta) / 10.0);
    const double est = 10.0 * std::log10(power);
    double common = 0.0;
    if (est > -options_.master_headroom_db) {
        common = floor_half(-options_.master_headroom_db - est);
        plan.notes.push_back({"Master", "estimated peak " + num(est) + " dBFS with the trims; " +
                                            num(-common) + " dB taken off every track to keep " +
                                            num(options_.master_headroom_db) + " dB headroom"});
    }

    for (Cand& c : cands) {
        const double total = c.delta + common;
        const double target_gain = std::clamp<double>(c.mix.gain_db + total, kMinGainDb, kMaxGainDb);
        if (std::fabs(target_gain - c.mix.gain_db) < options_.min_change_db) continue;
        SetMix op{c.mix};
        op.mix.gain_db = static_cast<float>(target_gain);
        std::string reason = "integrated " + num(c.a->lufs) + " LUFS, true peak " +
                             num(c.a->true_peak_dbtp) + " dBTP before the fader (" +
                             num(c.mix.gain_db) + " dB); target " + num(options_.target_lufs) +
                             " LUFS";
        if (!c.limit.empty()) reason += ", " + c.limit;
        if (common != 0.0) reason += ", " + num(common) + " dB for master headroom";
        const double change = target_gain - c.mix.gain_db;
        plan.steps.push_back({op,
                              std::string(change < 0 ? "Trim " : "Raise ") + c.track->name + " by " +
                                  num(std::fabs(change)) + " dB",
                              reason,
                              {}});
    }
    if (plan.steps.empty()) {
        error = "every track is within " + num(options_.min_change_db) + " dB of " +
                num(options_.target_lufs) + " LUFS";
        return false;
    }
    out = std::move(plan);
    return true;
}

// --- bus structure -------------------------------------------------------------------

bool BusStructurePlanner::propose(const Snapshot& at, Plan& out, std::string& error) {
    Plan plan;
    plan.label = "Bus Structure";
    plan.base_revision = at.revision;
    Id next = at.next_id;
    const Role order[] = {Role::Drums, Role::Bass, Role::Guitars, Role::Keys, Role::Vocals};
    std::size_t buses = at.edit->buses.size();

    for (const Role role : order) {
        std::vector<std::pair<const TrackEdit*, RoleGuess>> members;
        for (const auto& t : at.edit->tracks) {
            if (!is_audio(*t)) continue;
            RoleGuess g = classify_track(*at.edit, *t);
            if (g.role == role) members.emplace_back(t.get(), std::move(g));
        }
        if (members.size() < options_.min_tracks) {
            if (!members.empty())
                plan.notes.push_back({role_name(role), "only " + std::to_string(members.size()) +
                                                           " track: no bus"});
            continue;
        }
        const std::string name = role_name(role);
        const Bus* existing = find_bus_named(*at.edit, name);
        Id bus_id = 0;
        std::vector<std::size_t> needs;
        if (existing != nullptr) {
            bus_id = existing->id;
        } else {
            if (buses >= kMaxBuses) {
                plan.notes.push_back({name, "the session already has the most buses"});
                continue;
            }
            AddBus ab;
            ab.bus = next++;
            ab.name = name;
            bus_id = ab.bus;
            needs.push_back(plan.steps.size());
            plan.steps.push_back({ab, "Add " + name + " bus",
                                  std::to_string(members.size()) + " tracks are " + name +
                                      " by their names or inserts; one fader for the group",
                                  {}});
            ++buses;
        }
        for (const auto& [t, g] : members) {
            if (t->output == bus_id) continue;
            if (t->output != kMasterOutput) {
                plan.notes.push_back({"Track " + t->name, "already routed away from the master: "
                                                          "not moved to " + name});
                continue;
            }
            plan.steps.push_back({SetOutput{t->id, bus_id}, "Route " + t->name + " to " + name,
                                  g.why, needs});
        }
    }
    if (plan.steps.empty()) {
        error = "no group of tracks to bus: names, inserts and colours name no role with " +
                std::to_string(options_.min_tracks) + " tracks";
        return false;
    }
    out = std::move(plan);
    return true;
}

// --- parallel sends ---------------------------------------------------------------------

bool ParallelPlanner::propose(const Snapshot& at, Plan& out, std::string& error) {
    suggestions_.clear();
    Plan plan;
    plan.label = "Parallel Sends";
    plan.base_revision = at.revision;
    Id next = at.next_id;

    struct Strip {
        const TrackEdit* track;
        const TrackAnalysis* a;
        double gain_db;
    };
    const auto gather = [&](Role role) {
        std::vector<Strip> v;
        for (const auto& t : at.edit->tracks) {
            if (!is_audio(*t) || classify_track(*at.edit, *t).role != role) continue;
            const TrackMix* m = mix_of(at, t->id);
            const TrackAnalysis* a = analysis_.find(t->id);
            if (m == nullptr || a == nullptr || m->mute || !a->problem.empty() || a->silent())
                continue;
            v.push_back({t.get(), a, m->gain_db});
        }
        return v;
    };
    const auto add_aux = [&](const std::string& bus_name, const std::vector<Strip>& members,
                             float send_db, const std::string& why, ProcessorSetting proc) {
        if (find_bus_named(*at.edit, bus_name) != nullptr) {
            plan.notes.push_back({bus_name, "a bus of that name exists: not added again"});
            return;
        }
        AddBus ab;
        ab.bus = next++;
        ab.name = bus_name;
        const std::size_t bus_step = plan.steps.size();
        plan.steps.push_back({ab, "Add " + bus_name + " bus", why, {}});
        for (const Strip& m : members) {
            std::uint8_t slot = 0;
            if (!has_send_slot(*m.track, slot)) {
                plan.notes.push_back({"Track " + m.track->name, "no free send slot: no send to " +
                                                                   bus_name});
                continue;
            }
            AddSend as;
            as.strip = m.track->id;
            as.send = next++;
            as.slot = slot;
            as.bus = ab.bus;
            as.mix.level_db = send_db;
            plan.steps.push_back({as, "Send " + m.track->name + " to " + bus_name + " at " +
                                          num(send_db) + " dB",
                                  "post-fader send; " + why, {bus_step}});
        }
        plan.notes.push_back({bus_name, "put " + describe(proc) +
                                            " on the bus; aux buses cannot hold inserts yet"});
        suggestions_.push_back({bus_name, std::move(proc)});
    };

    const std::vector<Strip> drums = gather(Role::Drums);
    if (drums.size() >= 2) {
        double rms_power = 0.0;
        double peak = -200.0;
        for (const Strip& m : drums) {
            rms_power += std::pow(10.0, (m.a->rms_dbfs + m.gain_db) / 10.0);
            peak = std::max(peak, m.a->peak_dbfs + m.gain_db);
        }
        const double rms = 10.0 * std::log10(rms_power);
        const double crest = peak - rms;
        if (crest < 10.0) {
            plan.notes.push_back({"Drums", "crest " + num(crest) + " dB: already dense, no "
                                           "parallel compression"});
        } else {
            // Squash into the body of the kit; the more transient it is, the
            // less of the squashed copy it needs.
            const double threshold = std::clamp(round_half(rms - 3.0), -60.0, 0.0);
            const float send = static_cast<float>(std::clamp(round_half(-6.0 - (crest - 10.0) * 0.25),
                                                             -15.0, -6.0));
            ProcessorSetting p{"urn:sonora:native:compressor", "Sonora Compressor",
                               {pv("threshold", static_cast<float>(threshold)), pv("ratio", 8.0f),
                                pv("knee", 3.0f), pv("attack", 5.0f), pv("release", 80.0f),
                                pv("auto_makeup", 1.0f), pv("mix", 100.0f)}};
            add_aux("Drums Parallel", drums, send,
                    std::to_string(drums.size()) + " drum tracks, crest " + num(crest) +
                        " dB (peak " + num(peak) + ", RMS " + num(rms) + " dBFS): punch to keep, "
                        "body to add",
                    std::move(p));
        }
    } else {
        plan.notes.push_back({"Drums", "needs 2 drum tracks for parallel compression, found " +
                                           std::to_string(drums.size())});
    }

    const std::vector<Strip> bass = gather(Role::Bass);
    if (!bass.empty()) {
        ProcessorSetting p{"urn:sonora:native:saturator", "Sonora Saturator",
                           {pv("drive", 12.0f), pv("shape", 1.0f), pv("mode", 0.0f),
                            pv("focus", 800.0f), pv("tone", 0.0f), pv("mix", 100.0f),
                            pv("output", -6.0f)}};
        add_aux("Bass Saturation", bass, -12.0f,
                std::to_string(bass.size()) + " bass track" + (bass.size() == 1 ? "" : "s") +
                    ": harmonics make it audible on small speakers without raising its level",
                std::move(p));
    }

    if (plan.steps.empty()) {
        error = "nothing to send in parallel: no drums with transients and no bass track";
        return false;
    }
    out = std::move(plan);
    return true;
}

// --- joining and printing ------------------------------------------------------------------

bool parse_plan_kind(const std::string& word, MixPlanKind& out) {
    if (word == "gain") out = MixPlanKind::Gain;
    else if (word == "buses") out = MixPlanKind::Buses;
    else if (word == "parallel") out = MixPlanKind::Parallel;
    else if (word == "all") out = MixPlanKind::All;
    else return false;
    return true;
}

bool propose_mix_plan(MixPlanKind kind, const Snapshot& at, const MixAnalysis& analysis, Plan& out,
                      std::string& error) {
    GainStagingPlanner gain(analysis);
    BusStructurePlanner buses;
    ParallelPlanner parallel(analysis);
    std::vector<std::pair<const char*, PlanSource*>> run;
    if (kind == MixPlanKind::Gain || kind == MixPlanKind::All) run.emplace_back("Gain", &gain);
    if (kind == MixPlanKind::Buses || kind == MixPlanKind::All) run.emplace_back("Buses", &buses);
    if (kind == MixPlanKind::Parallel || kind == MixPlanKind::All)
        run.emplace_back("Parallel", &parallel);

    Plan joined;
    joined.base_revision = at.revision;
    Snapshot view = at;   // the same session; only the id counter moves on
    for (const auto& [name, src] : run) {
        Plan p;
        std::string why;
        if (!src->propose(view, p, why)) {
            if (run.size() == 1) {
                error = why;
                return false;
            }
            joined.notes.push_back({name, why});
            continue;
        }
        const std::size_t shift = joined.steps.size();
        for (PlanStep& s : p.steps) {
            for (std::size_t& n : s.needs) n += shift;
            joined.steps.push_back(std::move(s));
        }
        for (PlanNote& n : p.notes) joined.notes.push_back(std::move(n));
        joined.label = joined.label.empty() ? p.label : "Mix Assistant";
        view.next_id = next_free_id(view, joined);
    }
    if (joined.steps.empty()) {
        error = "nothing to propose";
        for (const PlanNote& n : joined.notes) error += "; " + n.subject + ": " + n.text;
        return false;
    }
    out = std::move(joined);
    return true;
}

bool format_plan(const Snapshot& at, const Plan& plan, std::string& text, std::string& error) {
    PlanPreview pv;
    if (!preview(at, plan, Selection::all(plan), pv, error)) return false;
    text = plan.label + " (" + std::to_string(plan.steps.size()) + " steps, one undo)\n";
    for (std::size_t i = 0; i < plan.steps.size(); ++i) {
        const PlanStep& s = plan.steps[i];
        text += std::to_string(i + 1) + ". " + s.title + "\n     why: " + s.reason + "\n";
        if (!s.needs.empty()) {
            text += "     needs:";
            for (const std::size_t n : s.needs) text += " " + std::to_string(n + 1);
            text += "\n";
        }
    }
    for (const PlanNote& n : plan.notes) text += "note " + n.subject + ": " + n.text + "\n";
    text += "changes:\n";
    for (const DiffLine& l : pv.lines) {
        const char mark = l.kind == DiffKind::Added ? '+' : l.kind == DiffKind::Removed ? '-' : '~';
        text += std::string("  ") + mark + " " + l.subject;
        if (!l.field.empty()) text += " " + l.field;
        if (l.kind == DiffKind::Changed) text += ": " + l.before + " -> " + l.after;
        text += "\n";
    }
    return true;
}

}  // namespace sonora
