// SPDX-License-Identifier: GPL-3.0-or-later
//
// tempo.edit detect (ADR-0114) with the analyze fake behind BeatRunner:
// the map is set from the beats and Undo Tempo restores it; a result below
// the confidence floor, or a failed detection, leaves the map alone.
#include <cstdio>
#include <memory>
#include <string>

#include "edit_controller.h"
#include "sonora/analyze/beat_fakes.h"
#include "sonora/ui_beat/beat_command.h"
#include "timeline_document.h"


using namespace sonora::ui;
using namespace sonora::ui::model;
namespace analyze = sonora::analyze;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

namespace {

class FakeRunner final : public BeatRunner {
public:
    analyze::FakeBeatDetector det;
    bool run(Id, analyze::BeatResult& out, std::string& error) override {
        ++calls;
        if (!det.fail_with.empty()) {
            error = det.fail_with;
            return false;
        }
        out = det.result;
        return true;
    }
    int calls = 0;
};

Snapshot session() {
    Snapshot s = make_empty_session(1, 48000);
    auto pool = std::make_shared<SourcePool>();
    Source take;
    take.id = 100;
    take.path = "Audio Files/Take 1.wav";
    take.length = 8 * 48000;
    pool->sources.push_back(take);
    s.sources = pool;
    auto edit = std::make_shared<EditState>(*s.edit);
    auto tr = std::make_shared<TrackEdit>(*edit->tracks[0]);
    auto p = std::make_shared<Playlist>(*tr->playlists[0]);
    Region r;
    r.id = 10;
    r.name = "Take";
    r.length = 8 * 48000;
    r.channels = {{100, 0}};
    p->regions.push_back(r);
    tr->playlists[0] = p;
    edit->tracks[0] = tr;
    s.edit = edit;
    s.next_id = 1000;
    return s;
}

}  // namespace

int main() {
    ModelDocument doc(session());
    EditController c(doc);
    FakeRunner run;
    std::string why;
    const auto original = doc.snapshot().settings.tempo;

    // Nothing selected: refused, the analyzer not even started.
    CHECK(!beat_command(c, &run, "detect", why));
    CHECK(run.calls == 0);

    CHECK(c.select_region(10));
    CHECK(!beat_command(c, &run, "detect banana", why) && why.find("detect [clip|selection]") != std::string::npos);
    CHECK(!beat_command(c, &run, "detect selection", why));  // no range
    CHECK(!beat_command(c, nullptr, "detect", why));
    CHECK(!beat_command(c, "detect", why));

    // Low confidence: refused with the reason, map untouched.
    run.det.result = analyze::fake_beat_result(100.0, 0.2);
    CHECK(!beat_command(c, &run, "detect", why));
    CHECK(why.find("uncertain") != std::string::npos);
    CHECK(doc.snapshot().settings.tempo == original);

    // A failed detection is an error, not a tempo.
    run.det.fail_with = "silent";
    CHECK(!beat_command(c, &run, "detect clip", why) && why.find("silent") != std::string::npos);
    CHECK(doc.snapshot().settings.tempo == original);
    run.det.fail_with.clear();

    // A steady result sets the map; follow is refused for it.
    run.det.result = analyze::fake_beat_result(100.0, 0.9);
    CHECK(!beat_command(c, &run, "detect follow", why));
    CHECK(doc.snapshot().settings.tempo == original);
    CHECK(beat_command(c, &run, "detect", why));
    const auto& map = doc.snapshot().settings.tempo;
    CHECK(!(map == original));
    CHECK(map.size() == 1 && map[0].bpm > 99.9f && map[0].bpm < 100.1f);

    // Undo Tempo puts the old map back.
    CHECK(c.tempo_undo(why));
    CHECK(doc.snapshot().settings.tempo == original);

    if (failures == 0) std::puts("ui_beat: ok");
    return failures == 0 ? 0 : 1;
}
