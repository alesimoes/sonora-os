// SPDX-License-Identifier: GPL-3.0-or-later
//
// Import Session Data panel's model, with fake planner, copier and applier:
// it lists tracks (greyed ones not tickable), passes the request flags and
// the ticked order to the planner, applies the Compound only after a
// successful copy, and holds a refusal or failed copy without applying.
#include <QCoreApplication>
#include <cstdio>

#include "session_import_model.h"

using namespace sonora;
using namespace sonora::ui;
using namespace sonora::session_files;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

struct FakePlanner : ImportPlanner {
    bool open_ok = true;
    bool plan_ok = true;
    SessionImportRequest seen;
    bool open_source(const std::string&, std::string& error) override {
        if (!open_ok) error = "not a session";
        return open_ok;
    }
    std::vector<ImportableTrack> tracks() override {
        ImportableTrack a; a.id = 1; a.name = "Kick";
        ImportableTrack b; b.id = 2; b.name = "Synth"; b.importable = false; b.why = "MIDI";
        ImportableTrack c; c.id = 3; c.name = "Snare";
        return {a, b, c};
    }
    bool plan(const SessionImportRequest& r, ImportPlan& p, ImportRefusal& f) override {
        seen = r;
        if (!plan_ok) { f.message = "Missing file"; return false; }
        p.dropped.push_back("dropped: send");
        return true;
    }
};
struct FakeCopier : ImportCopier {
    CopyOutcome result = CopyOutcome::Done;
    int calls = 0;
    CopyOutcome copy(const ImportPlan&, const std::atomic<bool>&, const CopyProgressFn&,
                     CopyReport& report) override {
        ++calls;
        if (result == CopyOutcome::Failed) report.message = "disk full";
        return result;
    }
};
struct FakeApplier : ImportApplier {
    bool take = false;
    int applied = 0;
    bool take_open() override { return take; }
    bool apply(const model::Compound&) override { ++applied; return true; }
};

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    FakePlanner planner;
    FakeCopier copier;
    FakeApplier applier;
    SessionImportModel m(planner, copier, applier);

    planner.open_ok = false;
    CHECK(!m.openSource("/x"));
    CHECK(!m.refusal().isEmpty());
    planner.open_ok = true;
    CHECK(m.openSource("/src"));
    CHECK(m.tracks().size() == 3);

    m.toggle(2);  // greyed: ignored
    CHECK(m.selectedCount() == 0);
    m.toggle(3);
    m.toggle(1);
    m.setMarkers(true);
    CHECK(m.startImport());
    CHECK(m.finishNow());
    CHECK(planner.seen.tracks == std::vector<model::Id>({3, 1}));
    CHECK(planner.seen.markers && planner.seen.source_folder == "/src");
    CHECK(applier.applied == 1);
    CHECK(m.notes().contains("send"));

    copier.result = CopyOutcome::Failed;
    CHECK(m.startImport());
    CHECK(!m.finishNow());
    CHECK(applier.applied == 1);  // nothing applied after a failed copy
    CHECK(m.refusal() == "disk full");

    copier.result = CopyOutcome::Cancelled;
    CHECK(m.startImport());
    m.finishNow();
    CHECK(applier.applied == 1);

    planner.plan_ok = false;
    const int before = copier.calls;
    CHECK(!m.startImport());
    CHECK(copier.calls == before && m.refusal() == "Missing file");

    planner.plan_ok = true;
    applier.take = true;
    m.startImport();
    m.finishNow();
    CHECK(planner.seen.take_open);
    return 0;
}
