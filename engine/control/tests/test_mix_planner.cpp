// SPDX-License-Identifier: GPL-3.0-or-later
//
// The mix assistant's rule-based planners (ADR-0045) on synthetic sessions
// with known levels: a loud snare, a quiet vocal, a kick and bass pair, a
// kit. Each plan is checked for its content, that the preview is what
// accepting leaves, and that one undo takes it all back.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "model_test.h"
#include "sonora/mix_planner.h"
#include "sonora/model/plan.h"
#include "sonora/wav_writer.h"

using namespace model_test;
namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kRate = 48000;
constexpr std::size_t kLen = kRate * 3;
const double kPi = 3.14159265358979323846;

using Signal = std::function<float(std::size_t)>;

// A sine of `amp` that sounds for `on` frames at the start of every `period`.
Signal bursts(double hz, double amp, std::size_t on, std::size_t period) {
    return [=](std::size_t i) {
        return i % period < on ? static_cast<float>(amp * std::sin(2.0 * kPi * hz * i / kRate))
                               : 0.0f;
    };
}
Signal tone(double hz, double amp) { return bursts(hz, amp, 1, 1); }

struct Rig {
    fs::path dir = fs::temp_directory_path() / "sonora_mix_planner_test";
    History h{new_session(settings())};
    Ids ids{h.current()};

    Rig() {
        fs::remove_all(dir);
        fs::create_directories(dir / "Audio Files");
    }
    ~Rig() { fs::remove_all(dir); }

    // Mono track with one region of the signal. Returns its id.
    Id add(const std::string& name, const Signal& s) {
        const AddTrack t = add_track(ids, name);
        if (!h.apply(t, err_)) return fail(err_);
        const AddSource src = add_source(ids, static_cast<Frames>(kLen), 1, t.track);
        if (!h.apply(src, err_)) return fail(err_);
        std::vector<float> data(kLen);
        for (std::size_t i = 0; i < kLen; ++i) data[i] = s(i);
        WavWriter w;
        if (!w.open((dir / src.source.path).string(), kRate, 1, WavWriter::Format::Float32, 0) ||
            !w.write(data.data(), kLen) || !w.close())
            return fail("cannot write the test file");
        PlaceRegion pr;
        pr.playlist = t.playlist;
        pr.region = region(ids(), src.source.id, 0, static_cast<Frames>(kLen));
        if (!h.apply(pr, err_)) return fail(err_);
        return t.track;
    }
    Id fail(const std::string& why) {
        std::fprintf(stderr, "rig: %s\n", why.c_str());
        return 0;
    }
    std::string err_;
};

double gain_of(const Snapshot& s, Id t) { return s.mix.find(t)->gain_db; }

const PlanStep* step_titled(const Plan& p, const char* prefix) {
    for (const PlanStep& s : p.steps)
        if (s.title.rfind(prefix, 0) == 0) return &s;
    return nullptr;
}

// preview == applied, and one undo takes the whole plan back.
int check_apply_and_undo(Rig& r, const Plan& plan, bool expect_step) {
    std::string error;
    CHECK(plan_problem(plan).empty());
    PlanPreview pv;
    CHECK(preview(r.h.current(), plan, Selection::all(plan), pv, error));
    const Snapshot before = r.h.current();
    const std::size_t depth = r.h.undo_depth();
    PlanReceipt rc;
    CHECK(accept_plan(r.h, plan, Selection::all(plan), rc, error));
    CHECK(pv.result == r.h.current());
    CHECK(r.h.current().revision == before.revision + 1);
    CHECK(r.h.undo_depth() == depth + (expect_step ? 1 : 0));
    CHECK(undo_plan(r.h, rc, error));
    CHECK(*r.h.current().edit == *before.edit);
    CHECK(r.h.current().mix.tracks == before.mix.tracks);
    CHECK(r.h.undo_depth() == depth);
    return 0;
}

int test_analysis() {
    Rig r;
    const Id snare = r.add("Snare", bursts(200, 0.9, 2400, 24000));
    const Id vocal = r.add("Vocal", tone(440, 0.01));
    CHECK(snare != 0 && vocal != 0);
    MixAnalysis a;
    std::string error;
    std::vector<double> seen;
    AnalysisHooks hooks;
    hooks.progress = [&](double f) { seen.push_back(f); };
    CHECK(analyse_session(r.h.current(), r.dir.string(), hooks, a, error));
    CHECK(a.tracks.size() == 2);
    const TrackAnalysis* s = a.find(snare);
    const TrackAnalysis* v = a.find(vocal);
    CHECK(s != nullptr && v != nullptr);
    CHECK(std::fabs(s->peak_dbfs - 20.0 * std::log10(0.9)) < 0.1);
    CHECK(std::fabs(v->peak_dbfs - (-40.0)) < 0.1);
    CHECK(std::fabs(v->rms_dbfs - (-43.0)) < 0.1);   // a sine's RMS is 3 dB under its peak
    CHECK(v->lufs > -45.0 && v->lufs < -41.0);
    CHECK(s->lufs > -8.0 && s->lufs < -1.0);   // gated: the silence between hits is left out
    CHECK(s->true_peak_dbtp >= s->peak_dbfs - 0.1);
    CHECK(!seen.empty() && seen.back() > 0.999);
    for (std::size_t i = 1; i < seen.size(); ++i) CHECK(seen[i] >= seen[i - 1]);

    // Cancelled before the first block: nothing is measured.
    std::atomic<bool> stop{true};
    AnalysisHooks cancel;
    cancel.cancel = &stop;
    CHECK(!analyse_session(r.h.current(), r.dir.string(), cancel, a, error));
    CHECK(mentions(error, "cancelled"));

    // A missing file costs its track, not the pass.
    fs::remove_all(r.dir / "Audio Files");
    CHECK(analyse_session(r.h.current(), r.dir.string(), AnalysisHooks{}, a, error));
    CHECK(!a.find(snare)->problem.empty());
    GainStagingPlanner gp(a);
    Plan p;
    CHECK(!gp.propose(r.h.current(), p, error));
    CHECK(mentions(error, "no track has audio"));
    return 0;
}

int test_gain_staging() {
    Rig r;
    const Id snare = r.add("Snare", bursts(200, 0.9, 2400, 24000));
    const Id vocal = r.add("Vocal", tone(440, 0.01));
    CHECK(snare != 0 && vocal != 0);
    MixAnalysis a;
    std::string error;
    CHECK(analyse_session(r.h.current(), r.dir.string(), AnalysisHooks{}, a, error));

    GainStagingPlanner gp(a);
    Plan plan;
    CHECK(gp.propose(r.h.current(), plan, error));
    CHECK(plan.steps.size() == 2);
    const PlanStep* trim = step_titled(plan, "Trim Snare");
    const PlanStep* raise = step_titled(plan, "Raise Vocal");
    CHECK(trim != nullptr && raise != nullptr);
    const float snare_db = std::get<SetMix>(trim->op).mix.gain_db;
    const float vocal_db = std::get<SetMix>(raise->op).mix.gain_db;
    // Both land on the target: the loud snare comes down about 19 dB, the
    // quiet vocal up about 20.
    CHECK(snare_db < -15.0f && snare_db > -23.0f);
    CHECK(vocal_db > 16.0f && vocal_db < 24.0f);
    const TrackAnalysis* sa = a.find(snare);
    const TrackAnalysis* va = a.find(vocal);
    CHECK(std::fabs(sa->lufs + snare_db - (-23.0)) <= 0.5 + 1e-6);
    CHECK(std::fabs(va->lufs + vocal_db - (-23.0)) <= 0.5 + 1e-6);
    CHECK(mentions(trim->reason, "LUFS") && mentions(trim->reason, "true peak"));
    CHECK(trim->needs.empty());

    // Deterministic.
    Plan again;
    CHECK(gp.propose(r.h.current(), again, error));
    CHECK(again.steps.size() == plan.steps.size() &&
          std::get<SetMix>(again.steps[0].op).mix.gain_db ==
              std::get<SetMix>(plan.steps[0].op).mix.gain_db);

    CHECK(check_apply_and_undo(r, plan, false) == 0);
    CHECK(gain_of(r.h.current(), snare) == 0.0f);

    // After the trims there is nothing to do: not an empty success.
    PlanReceipt rc;
    CHECK(accept_plan(r.h, plan, Selection::all(plan), rc, error));
    MixAnalysis b = a;   // the audio is unchanged, the faders moved
    GainStagingPlanner settled(b);
    CHECK(!settled.propose(r.h.current(), plan, error));
    CHECK(mentions(error, "within"));
    return 0;
}

int test_gain_staging_limits() {
    Rig r;
    // Peaky: a boost to the target would put its peak over the ceiling.
    const Id click = r.add("Click", bursts(1000, 0.5, 48, 24000));
    // Muted tracks are left alone, with a note.
    const Id quiet = r.add("Quiet", tone(440, 0.01));
    CHECK(click != 0 && quiet != 0);
    SetMix m{*r.h.current().mix.find(quiet)};
    m.mix.mute = true;
    CHECK_APPLY(r.h, m);
    MixAnalysis a;
    std::string error;
    CHECK(analyse_session(r.h.current(), r.dir.string(), AnalysisHooks{}, a, error));
    Plan plan;
    GainStagingPlanner gp(a);
    CHECK(gp.propose(r.h.current(), plan, error));
    bool muted_noted = false;
    for (const PlanNote& n : plan.notes) muted_noted = muted_noted || n.subject == "Track Quiet";
    CHECK(muted_noted && step_titled(plan, "Raise Quiet") == nullptr);
    const TrackAnalysis* ca = a.find(click);
    for (const PlanStep& s : plan.steps) {
        const float g = std::get<SetMix>(s.op).mix.gain_db;
        CHECK(ca->true_peak_dbtp + g <= -3.0 + 1e-6);   // never over the ceiling
    }

    // The master: a headroom no sum of tracks can keep is met by a common
    // trim, and the plan says so.
    GainStagingOptions strict;
    strict.master_headroom_db = 30.0;
    Rig r2;
    r2.add("A", tone(440, 0.5));
    r2.add("B", tone(660, 0.5));
    CHECK(analyse_session(r2.h.current(), r2.dir.string(), AnalysisHooks{}, a, error));
    GainStagingPlanner sp(a, strict);
    CHECK(sp.propose(r2.h.current(), plan, error));
    bool master_noted = false;
    for (const PlanNote& n : plan.notes) master_noted = master_noted || n.subject == "Master";
    CHECK(master_noted);
    double power = 0.0;
    for (const PlanStep& s : plan.steps) {
        const auto& sm = std::get<SetMix>(s.op);
        power += std::pow(10.0, (a.find(sm.mix.track)->true_peak_dbtp + sm.mix.gain_db) / 10.0);
    }
    CHECK(10.0 * std::log10(power) <= -30.0 + 1e-6);
    return 0;
}

int test_roles() {
    Rig r;
    const Id kick = r.add("Kick In", tone(60, 0.1));
    const Id oh = r.add("OH L", tone(60, 0.1));
    const Id bassg = r.add("Bass Guitar", tone(60, 0.1));
    const Id vox = r.add("Lead Vox", tone(60, 0.1));
    const Id ohm = r.add("Ohm", tone(60, 0.1));
    const Id gtr = r.add("Gtr 2", tone(60, 0.1));
    const Id tr7 = r.add("Track 7", tone(60, 0.1));
    const Id plug = r.add("Track 8", tone(60, 0.1));
    CHECK(plug != 0);
    CHECK_APPLY(r.h, (SetTrackColour{gtr, 3}));
    CHECK_APPLY(r.h, (SetTrackColour{tr7, 3}));
    AddInsert de;
    de.track = plug;
    de.insert = r.ids();
    de.slot = 0;
    de.plugin = PluginRef{PluginFormat::Lv2, "urn:sonora:native:deesser", "Sonora De-esser", "1"};
    CHECK_APPLY(r.h, de);

    const EditState& e = *r.h.current().edit;
    const auto of = [&](Id t) {
        for (const auto& tr : e.tracks)
            if (tr->id == t) return classify_track(e, *tr);
        return RoleGuess{};
    };
    CHECK(of(kick).role == Role::Drums && mentions(of(kick).why, "kick"));
    CHECK(of(oh).role == Role::Drums);
    CHECK(of(bassg).role == Role::Bass);   // not guitars
    CHECK(of(vox).role == Role::Vocals);
    CHECK(of(ohm).role == Role::Unknown);
    CHECK(of(gtr).role == Role::Guitars);
    CHECK(of(tr7).role == Role::Guitars && mentions(of(tr7).why, "same colour as \"Gtr 2\""));
    CHECK(of(plug).role == Role::Vocals && mentions(of(plug).why, "de-esser"));
    return 0;
}

int test_bus_structure() {
    Rig r;
    const Id kick = r.add("Kick", tone(60, 0.1));
    const Id snare = r.add("Snare", tone(200, 0.1));
    const Id bass = r.add("Bass", tone(55, 0.1));
    const Id vox = r.add("Vox", tone(440, 0.1));
    CHECK(vox != 0);
    std::string error;
    BusStructurePlanner bp;
    Plan plan;
    CHECK(bp.propose(r.h.current(), plan, error));
    // Only the drums have two tracks: a bus and two routes.
    CHECK(plan.steps.size() == 3);
    CHECK(plan.steps[0].title == "Add Drums bus");
    CHECK(std::holds_alternative<AddBus>(plan.steps[0].op));
    for (std::size_t i = 1; i < 3; ++i) {
        CHECK(std::holds_alternative<SetOutput>(plan.steps[i].op));
        CHECK((plan.steps[i].needs == std::vector<std::size_t>{0}));
        CHECK(mentions(plan.steps[i].reason, "matches"));
    }
    CHECK(plan.steps[1].title == "Route Kick to Drums");
    bool single_noted = false;
    for (const PlanNote& n : plan.notes) single_noted = single_noted || n.subject == "Bass";
    CHECK(single_noted);

    // Rejecting the bus drops the routes, and the preview shows exactly that.
    Selection sel = Selection::all(plan);
    sel.accepted[0] = false;
    PlanPreview pv;
    CHECK(preview(r.h.current(), plan, sel, pv, error));
    CHECK(pv.resolved.applied.empty() && pv.resolved.dropped.size() == 2);

    CHECK(check_apply_and_undo(r, plan, true) == 0);
    PlanReceipt rc;
    CHECK(accept_plan(r.h, plan, Selection::all(plan), rc, error));
    const Id drums = r.h.current().edit->buses[0].id;
    for (const auto& t : r.h.current().edit->tracks) {
        const bool drum = t->id == kick || t->id == snare;
        CHECK(t->output == (drum ? drums : kMasterOutput));
    }
    (void)bass;
    // Done already: nothing more to propose, and a bus that exists is reused.
    CHECK(!bp.propose(r.h.current(), plan, error));
    const Id hat = r.add("Hat", tone(6000, 0.1));
    CHECK(hat != 0);
    CHECK(bp.propose(r.h.current(), plan, error));
    CHECK(plan.steps.size() == 1 && std::get<SetOutput>(plan.steps[0].op).output == drums);
    CHECK(plan.steps[0].needs.empty());
    return 0;
}

int test_kick_and_bass() {
    Rig r;
    const Id kick = r.add("Kick", bursts(60, 0.8, 4800, 24000));
    const Id bass = r.add("Bass", tone(55, 0.3));
    CHECK(kick != 0 && bass != 0);
    MixAnalysis a;
    std::string error;
    CHECK(analyse_session(r.h.current(), r.dir.string(), AnalysisHooks{}, a, error));

    // One of each: no bus by role, no parallel compression; the bass gets a
    // saturation aux.
    Plan plan;
    BusStructurePlanner bp;
    CHECK(!bp.propose(r.h.current(), plan, error));
    CHECK(mentions(error, "no group of tracks"));

    ParallelPlanner pp(a);
    CHECK(pp.propose(r.h.current(), plan, error));
    CHECK(plan.steps.size() == 2);
    CHECK(plan.steps[0].title == "Add Bass Saturation bus");
    const auto& send = std::get<AddSend>(plan.steps[1].op);
    CHECK(send.strip == bass && send.bus == std::get<AddBus>(plan.steps[0].op).bus);
    CHECK(send.mix.level_db == -12.0f && !send.mix.pre);
    CHECK((plan.steps[1].needs == std::vector<std::size_t>{0}));
    CHECK(pp.suggestions().size() == 1);
    CHECK(pp.suggestions()[0].processor.uri == "urn:sonora:native:saturator");
    bool drums_noted = false;
    for (const PlanNote& n : plan.notes) drums_noted = drums_noted || n.subject == "Drums";
    CHECK(drums_noted);
    CHECK(check_apply_and_undo(r, plan, true) == 0);

    // The gain plan on the pair brings both to the target.
    GainStagingPlanner gp(a);
    CHECK(gp.propose(r.h.current(), plan, error));
    CHECK(check_apply_and_undo(r, plan, false) == 0);
    return 0;
}

int test_parallel_drums_and_all() {
    Rig r;
    const Id kick = r.add("Kick", bursts(60, 0.8, 1200, 24000));
    const Id snare = r.add("Snare", bursts(200, 0.9, 1200, 24000));
    const Id hat = r.add("Hat", bursts(6000, 0.5, 500, 12000));
    const Id bass = r.add("Bass", tone(55, 0.3));
    CHECK(kick != 0 && snare != 0 && hat != 0 && bass != 0);
    MixAnalysis a;
    std::string error;
    CHECK(analyse_session(r.h.current(), r.dir.string(), AnalysisHooks{}, a, error));

    ParallelPlanner pp(a);
    Plan plan;
    CHECK(pp.propose(r.h.current(), plan, error));
    // Drums Parallel: bus + 3 sends; Bass Saturation: bus + 1 send.
    CHECK(plan.steps.size() == 6);
    CHECK(plan.steps[0].title == "Add Drums Parallel bus");
    CHECK(mentions(plan.steps[0].reason, "crest"));
    const float level = std::get<AddSend>(plan.steps[1].op).mix.level_db;
    CHECK(level >= -15.0f && level <= -6.0f);
    for (std::size_t i = 2; i <= 3; ++i) CHECK(std::get<AddSend>(plan.steps[i].op).mix.level_db == level);
    CHECK(pp.suggestions().size() == 2);
    const ProcessorSetting& comp = pp.suggestions()[0].processor;
    CHECK(comp.uri == "urn:sonora:native:compressor" && comp.name == "Sonora Compressor");
    float ratio = 0, threshold = 0;
    for (const ParamValue& v : comp.params) {
        if (v.key == "ratio") ratio = v.value;
        if (v.key == "threshold") threshold = v.value;
    }
    CHECK(ratio == 8.0f && threshold < 0.0f && threshold >= -60.0f);
    CHECK(check_apply_and_undo(r, plan, true) == 0);

    // The whole assistant: gain, buses and parallel in one plan, ids carried
    // on, dependencies re-indexed, one undo step for the lot.
    Plan all;
    CHECK(propose_mix_plan(MixPlanKind::All, r.h.current(), a, all, error));
    CHECK(all.label == "Mix Assistant");
    CHECK(std::holds_alternative<SetMix>(all.steps[0].op));
    for (std::size_t i = 0; i < all.steps.size(); ++i)
        for (const std::size_t n : all.steps[i].needs) CHECK(n < i);
    CHECK(all.steps.size() > plan.steps.size());
    CHECK(check_apply_and_undo(r, all, true) == 0);

    // Partial: reject one drum's route only; the rest still applies.
    std::size_t route = 0;
    for (std::size_t i = 0; i < all.steps.size(); ++i)
        if (all.steps[i].title == "Route Hat to Drums") route = i;
    CHECK(route != 0);
    Selection sel = Selection::all(all);
    sel.accepted[route] = false;
    PlanPreview pv;
    CHECK(preview(r.h.current(), all, sel, pv, error));
    CHECK(pv.resolved.dropped.empty() && pv.resolved.applied.size() == all.steps.size() - 1);

    // The text preview.
    std::string text;
    CHECK(format_plan(r.h.current(), all, text, error));
    CHECK(mentions(text, "Trim ") && mentions(text, "why:") && mentions(text, "+ Bus Drums") &&
          mentions(text, "gain_db") && mentions(text, "Mix Assistant"));

    // Dense drums need no parallel copy.
    Rig dense;
    dense.add("Kick", tone(60, 0.5));
    dense.add("Snare", tone(200, 0.5));
    CHECK(analyse_session(dense.h.current(), dense.dir.string(), AnalysisHooks{}, a, error));
    ParallelPlanner dp(a);
    CHECK(!dp.propose(dense.h.current(), plan, error));
    CHECK(mentions(error, "nothing to send"));
    return 0;
}

int test_stale_plan_refused() {
    Rig r;
    r.add("Snare", bursts(200, 0.9, 2400, 24000));
    MixAnalysis a;
    std::string error;
    CHECK(analyse_session(r.h.current(), r.dir.string(), AnalysisHooks{}, a, error));
    Plan plan;
    CHECK(propose_mix_plan(MixPlanKind::Gain, r.h.current(), a, plan, error));
    CHECK_APPLY(r.h, (SetTrackName{r.h.current().edit->tracks[0]->id, "Snare 2"}));
    PlanReceipt rc;
    CHECK(!accept_plan(r.h, plan, Selection::all(plan), rc, error));
    CHECK(mentions(error, "changed since"));
    MixPlanKind k;
    CHECK(parse_plan_kind("parallel", k) && k == MixPlanKind::Parallel && !parse_plan_kind("x", k));
    return 0;
}

}  // namespace

int main() {
    if (test_analysis() != 0) return 1;
    if (test_gain_staging() != 0) return 1;
    if (test_gain_staging_limits() != 0) return 1;
    if (test_roles() != 0) return 1;
    if (test_bus_structure() != 0) return 1;
    if (test_kick_and_bass() != 0) return 1;
    if (test_parallel_drums_and_all() != 0) return 1;
    if (test_stale_plan_refused() != 0) return 1;
    std::puts("test_mix_planner OK");
    return 0;
}
