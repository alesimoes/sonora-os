// SPDX-License-Identifier: GPL-3.0-or-later
//
// Plans (ADR-0038): the diff preview is what accept applies, one undo takes
// the whole plan back (structure and fader values), the operator can accept
// part of it, and a plan that cannot be taken back or is stale is refused.
#include "model_test.h"
#include "sonora/model/plan.h"

using namespace model_test;

namespace {

// A rule-based pass in miniature: a shared reverb return, a send from every
// track, and a 3 dB trim of the first track. Ids are allocated up front.
class FakePlanSource : public PlanSource {
public:
    bool propose(const Snapshot& at, Plan& out, std::string& error) override {
        if (at.edit->tracks.empty()) {
            error = "the session has no tracks";
            return false;
        }
        Ids ids(at);
        Plan p;
        p.label = "Reverb Plan";
        p.base_revision = at.revision;
        AddBus bus;
        bus.bus = ids();
        bus.name = "Reverb";
        p.steps.push_back({bus, "Add reverb return", "shared tail", {}});
        for (const auto& t : at.edit->tracks) {
            AddSend s;
            s.strip = t->id;
            s.send = ids();
            s.slot = 0;
            s.bus = bus.bus;
            s.mix.level_db = -12.0f;
            p.steps.push_back({s, "Reverb send on " + t->name, "excess check passed", {0}});
        }
        SetMix trim{*at.mix.find(at.edit->tracks[0]->id)};
        trim.mix.gain_db -= 3.0f;
        p.steps.push_back({trim, "Trim " + at.edit->tracks[0]->name, "peak at -1 dBFS", {}});
        p.notes.push_back({"Vox insert 1", "third-party plugin: not adjusted"});
        out = std::move(p);
        return true;
    }
};

struct Session {
    History h{new_session(settings())};
    Id vox = 0, gtr = 0;
};

int make(Session& s) {
    Ids ids(s.h.current());
    const AddTrack v = add_track(ids, "Vox");
    const AddTrack g = add_track(ids, "Gtr");
    s.vox = v.track;
    s.gtr = g.track;
    CHECK_APPLY(s.h, v);
    CHECK_APPLY(s.h, g);
    return 0;
}

std::size_t sends_on(const Snapshot& s, Id track) {
    for (const auto& t : s.edit->tracks)
        if (t->id == track) return t->sends.size();
    return 0;
}

bool has_line(const PlanPreview& p, DiffKind k, const char* subject, const char* field) {
    for (const DiffLine& l : p.lines)
        if (l.kind == k && l.subject == subject && l.field == field) return true;
    return false;
}

int test_preview_matches_applied() {
    Session s;
    CHECK(make(s) == 0);
    FakePlanSource src;
    Plan plan;
    std::string error;
    CHECK(src.propose(s.h.current(), plan, error));
    CHECK(plan_problem(plan).empty());

    PlanPreview pv;
    CHECK(preview(s.h.current(), plan, Selection::all(plan), pv, error));
    CHECK(has_line(pv, DiffKind::Added, "Bus Reverb", ""));
    CHECK(has_line(pv, DiffKind::Added, "Track Vox", "send"));
    CHECK(has_line(pv, DiffKind::Added, "Track Gtr", "send"));
    CHECK(has_line(pv, DiffKind::Changed, "Track Vox", "gain_db"));
    for (const DiffLine& l : pv.lines)
        if (l.field == "gain_db") CHECK(l.before == "0" && l.after == "-3");

    const Snapshot before = s.h.current();
    PlanReceipt r;
    CHECK(accept_plan(s.h, plan, Selection::all(plan), r, error));
    CHECK(pv.result == s.h.current());
    CHECK(s.h.current().revision == before.revision + 1);   // one revision
    CHECK(s.h.undo_label() == "Reverb Plan");
    return 0;
}

int test_one_undo_reverts_all() {
    Session s;
    CHECK(make(s) == 0);
    FakePlanSource src;
    Plan plan;
    std::string error;
    CHECK(src.propose(s.h.current(), plan, error));
    const Snapshot before = s.h.current();
    const std::size_t depth = s.h.undo_depth();

    PlanReceipt r;
    CHECK(accept_plan(s.h, plan, Selection::all(plan), r, error));
    CHECK(s.h.undo_depth() == depth + 1);   // one step for the whole plan
    CHECK(s.h.current().edit->buses.size() == 1);
    CHECK(s.h.current().mix.find(s.vox)->gain_db == -3.0f);

    CHECK(undo_plan(s.h, r, error));
    CHECK(*s.h.current().edit == *before.edit);
    CHECK(s.h.current().edit->buses.empty());
    CHECK(s.h.current().mix.find(s.vox)->gain_db == 0.0f);   // values come back too
    CHECK(s.h.undo_depth() == depth);

    // Once something else has happened, the receipt is refused, not applied.
    CHECK(accept_plan(s.h, plan, Selection::all(plan), r, error) == false);   // stale revision
    CHECK(mentions(error, "changed since"));
    CHECK(src.propose(s.h.current(), plan, error));
    CHECK(accept_plan(s.h, plan, Selection::all(plan), r, error));
    CHECK_APPLY(s.h, (SetTrackName{s.gtr, "Guitar"}));
    CHECK(!undo_plan(s.h, r, error));
    CHECK(mentions(error, "latest change"));
    return 0;
}

int test_mix_only_plan_undoes() {
    Session s;
    CHECK(make(s) == 0);
    Plan plan;
    plan.label = "Gain Staging";
    plan.base_revision = s.h.current().revision;
    SetMix a{*s.h.current().mix.find(s.vox)};
    a.mix.gain_db = -6.0f;
    SetMix b = a;
    b.mix.gain_db = -9.0f;   // a later step on the same strip
    plan.steps.push_back({a, "Vox to -6", "", {}});
    plan.steps.push_back({b, "Vox to -9", "", {}});
    const std::size_t depth = s.h.undo_depth();
    PlanReceipt r;
    std::string error;
    CHECK(accept_plan(s.h, plan, Selection::all(plan), r, error));
    CHECK(!r.pushed_step && s.h.undo_depth() == depth);
    CHECK(s.h.current().mix.find(s.vox)->gain_db == -9.0f);
    CHECK(undo_plan(s.h, r, error));
    CHECK(s.h.current().mix.find(s.vox)->gain_db == 0.0f);
    return 0;
}

int test_partial_acceptance() {
    Session s;
    CHECK(make(s) == 0);
    FakePlanSource src;
    Plan plan;
    std::string error;
    CHECK(src.propose(s.h.current(), plan, error));   // 0 bus, 1 Vox send, 2 Gtr send, 3 trim

    // Reject the Gtr send and the trim: the bus and the Vox send remain.
    Selection sel = Selection::all(plan);
    sel.accepted[2] = false;
    sel.accepted[3] = false;
    PlanPreview pv;
    CHECK(preview(s.h.current(), plan, sel, pv, error));
    CHECK((pv.resolved.applied == std::vector<std::size_t>{0, 1}));
    CHECK(!has_line(pv, DiffKind::Added, "Track Gtr", "send"));
    CHECK(!has_line(pv, DiffKind::Changed, "Track Vox", "gain_db"));
    PlanReceipt r;
    CHECK(accept_plan(s.h, plan, sel, r, error));
    CHECK(pv.result == s.h.current());
    CHECK(sends_on(s.h.current(), s.vox) == 1 && sends_on(s.h.current(), s.gtr) == 0);
    CHECK(s.h.current().mix.find(s.vox)->gain_db == 0.0f);
    CHECK(undo_plan(s.h, r, error));
    CHECK(s.h.current().edit->buses.empty());

    // Reject the bus: both sends depend on it and are dropped, loudly.
    CHECK(src.propose(s.h.current(), plan, error));
    sel = Selection::all(plan);
    sel.accepted[0] = false;
    CHECK(preview(s.h.current(), plan, sel, pv, error));
    CHECK((pv.resolved.applied == std::vector<std::size_t>{3}));
    CHECK((pv.resolved.dropped == std::vector<std::size_t>{1, 2}));

    // Accepting nothing applies nothing.
    const Snapshot before = s.h.current();
    CHECK(accept_plan(s.h, plan, Selection::none(), r, error));
    CHECK(s.h.current() == before);
    CHECK(undo_plan(s.h, r, error));
    CHECK(s.h.current() == before);
    return 0;
}

int test_refusals() {
    Session s;
    CHECK(make(s) == 0);
    FakePlanSource src;
    Plan plan;
    std::string error;
    CHECK(src.propose(s.h.current(), plan, error));

    // A member a plan cannot take back.
    Plan bad = plan;
    bad.steps.push_back({SetSettings{}, "Change tempo", "", {}});
    CHECK(!plan_problem(bad).empty());
    PlanPreview pv;
    CHECK(!preview(s.h.current(), bad, Selection::all(bad), pv, error));
    CHECK(mentions(error, "cannot take this operation back"));

    // A dependency that is not earlier.
    bad = plan;
    bad.steps[1].needs = {2};
    CHECK(!plan_problem(bad).empty());

    // The engine's check refuses a step: the whole plan is refused, named.
    const MemberCheck no_sends = [](const Snapshot&, MemberRef m) -> std::string {
        return m.holds<AddSend>() ? "no more sends" : std::string();
    };
    const Snapshot before = s.h.current();
    PlanReceipt r;
    CHECK(!accept_plan(s.h, plan, Selection::all(plan), r, error, no_sends));
    CHECK(mentions(error, "step 2 (Reverb send on Vox)") && mentions(error, "no more sends"));
    CHECK(s.h.current() == before);

    // A source with nothing to say says so.
    History empty(new_session(settings()));
    CHECK(!src.propose(empty.current(), plan, error));
    return 0;
}

}  // namespace

int main() {
    if (test_preview_matches_applied() != 0) return 1;
    if (test_one_undo_reverts_all() != 0) return 1;
    if (test_mix_only_plan_undoes() != 0) return 1;
    if (test_partial_acceptance() != 0) return 1;
    if (test_refusals() != 0) return 1;
    std::puts("test_model_plan OK");
    return 0;
}
