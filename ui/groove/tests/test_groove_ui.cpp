// SPDX-License-Identifier: GPL-3.0-or-later
//
// Groove as an edit (ADR-0115): a swing moves the off-beat regions in one
// undo step, a groove taken from a reference track's onsets, a move that
// would overlap a neighbour left in place and reported, saved templates and
// the corrupt file that must be reported, the peak-envelope onsets.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "edit_controller.h"
#include "peak_source.h"
#include "sonora/ui_groove/groove_command.h"
#include "sonora/ui_groove/groove_panel.h"
#include "sonora/ui_groove/onset_source.h"
#include "sonora/ui_groove/template_store.h"
#include "timeline_document.h"

using namespace sonora;
using namespace sonora::ui;
using namespace sonora::ui::model;

#define OK(cond)                                                                 \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                            \
        }                                                                        \
    } while (0)

namespace {

constexpr uint32_t kRate = 48000;
constexpr Frames kBeat = kRate / 2;    // 120 bpm, the default map
constexpr Frames kSlot = kBeat / 4;    // a sixteenth, 6000 frames
constexpr Frames kBar = 4 * kBeat;

namespace fs = std::filesystem;

// `layout[t]` is track t's regions as (position, length), ids from 10 up.
Snapshot session(const std::vector<std::vector<std::pair<Frames, Frames>>>& layout) {
    Snapshot s = make_empty_session(static_cast<uint32_t>(layout.size()), kRate);
    auto pool = std::make_shared<SourcePool>();
    Source take;
    take.id = 100;
    take.path = "Audio Files/Take 1.wav";
    take.length = 600 * kRate;
    pool->sources.push_back(take);
    s.sources = pool;
    auto edit = std::make_shared<EditState>(*s.edit);
    Id next = 10;
    for (std::size_t t = 0; t < layout.size(); ++t) {
        auto tr = std::make_shared<TrackEdit>(*edit->tracks[t]);
        auto p = std::make_shared<Playlist>(*tr->playlists[0]);
        for (const auto& [pos, len] : layout[t]) {
            Region r;
            r.id = next++;
            r.name = "R" + std::to_string(r.id);
            r.position = pos;
            r.length = len;
            r.source_offset = pos;
            r.channels = {{100, 0}};
            p->regions.push_back(r);
        }
        tr->playlists[0] = p;
        edit->tracks[t] = tr;
    }
    s.edit = edit;
    s.next_id = 1000;
    return s;
}

std::vector<Frames> positions(const TimelineDocument& d, std::size_t track) {
    std::vector<Frames> out;
    for (const Region& r : active_playlist(d.snapshot().edit->tracks[track].get())->regions) {
        out.push_back(r.position);
    }
    return out;
}

// Eight sixteenths from bar 1, each half a slot long.
std::vector<std::pair<Frames, Frames>> sixteenths(Frames length = kSlot / 2) {
    std::vector<std::pair<Frames, Frames>> v;
    for (int i = 0; i < 8; ++i) v.emplace_back(kBar + i * kSlot, length);
    return v;
}

bool near(Frames a, Frames b) { return a - b <= 1 && b - a <= 1; }

std::string scratch_file() {
    std::random_device rd;
    return (fs::temp_directory_path() /
            ("sonora-groove-test-" + std::to_string(rd()) + "-" + std::to_string(rd()) + ".json"))
        .string();
}

int test_swing_and_undo() {
    ModelDocument doc(session({sixteenths()}));
    EditController c(doc);
    c.set_mode(EditMode::Slip);
    c.select_range(0, kBar, kBar + 2 * kBeat);
    const auto before = positions(doc, 0);
    const uint64_t rev = doc.snapshot().revision;
    std::string why;
    // 66 percent: the second sixteenth of each pair starts 0.32 of a slot late.
    OK(groove_command(c, GrooveDeps{}, "groove swing 66 16", why));
    OK(doc.snapshot().revision == rev + 1);  // one Compound
    const auto after = positions(doc, 0);
    const Frames delay = kSlot * 32 / 100;
    for (std::size_t i = 0; i < 8; ++i) {
        OK(near(after[i], before[i] + (i % 2 ? delay : 0)));
    }
    std::string error;
    OK(check_invariants(doc.snapshot(), error));
    // One Undo puts every region back.
    OK(c.undo(why));
    OK(positions(doc, 0) == before);
    OK(c.redo(why) && positions(doc, 0) == after);

    // Half the strength, half the delay.
    OK(c.undo(why));
    OK(groove_command(c, GrooveDeps{}, "swing 66 16 50", why));
    OK(near(positions(doc, 0)[1], before[1] + delay / 2));
    return 0;
}

int test_from_reference() {
    // Track 0 is a loop whose fake onsets lean late on the off sixteenths by
    // a quarter of a slot; track 1 is grooved to it.
    ModelDocument doc(session({{{kBar, kBar}}, sixteenths()}));
    EditController c(doc);
    c.set_mode(EditMode::Slip);
    FixedOnsetSource onsets;
    std::vector<Frames> hits;
    for (int i = 0; i < 16; ++i) hits.push_back(kBar + i * kSlot + (i % 2 ? kSlot / 4 : 0));
    onsets.set(10, hits);
    GrooveDeps deps;
    deps.onsets = &onsets;
    std::string why;

    // The reference alone, with nothing below it, is refused.
    c.select_range(0, kBar, 2 * kBar, 1);
    OK(!groove_command(c, deps, "groove region", why) && why.find("one more track") != std::string::npos);

    c.select_range(0, kBar, 2 * kBar, 2);
    const auto ref_before = positions(doc, 0);
    const auto before = positions(doc, 1);
    OK(groove_command(c, deps, "groove region 4", why));
    const auto after = positions(doc, 1);
    for (std::size_t i = 0; i < 8; ++i) {
        OK(near(after[i], before[i] + (i % 2 ? kSlot / 4 : 0)));
    }
    OK(positions(doc, 0) == ref_before);  // the reference stays where it is

    // Without an onset source (the tempo.edit entry), refused, nothing moved.
    OK(c.undo(why));
    OK(!groove_command(c, GrooveDeps{}, "groove region", why) &&
       why.find("waveform overview") != std::string::npos);
    OK(positions(doc, 1) == before);
    return 0;
}

int test_overlap_reported() {
    // R10 moves; R11 would run into R12, which is outside the range and stays.
    const Frames start = kBar + kSlot;
    ModelDocument doc(session({{{start, kSlot / 2},
                                {kBar + 3 * kSlot, kSlot - 1000},
                                {kBar + 4 * kSlot, kSlot}}}));
    EditController c(doc);
    c.set_mode(EditMode::Slip);
    c.select_range(0, start, kBar + 4 * kSlot);
    const auto before = positions(doc, 0);
    groove::GrooveTemplate swing;
    std::string why;
    OK(groove::groove_swing(66, 4, swing, why));
    GrooveReport report;
    OK(groove_apply(c, swing, 100, report, why));
    OK(report.moved == 1 && report.overlapping == 1);
    OK(report.message.find("1 was left in place") != std::string::npos);
    auto after = positions(doc, 0);
    OK(after[0] != before[0] && after[1] == before[1] && after[2] == before[2]);

    // The text entry says so: a region left behind must not pass unannounced.
    OK(c.undo(why));
    OK(!groove_command(c, GrooveDeps{}, "groove swing 66 16", why));
    OK(why.find("Groove moved 1 region") != std::string::npos);
    OK(why.find("overlap") != std::string::npos);
    OK(positions(doc, 0)[0] != before[0]);

    // Everything blocked: refused, and nothing changed.
    OK(c.undo(why));
    c.select_range(0, kBar + 3 * kSlot, kBar + 4 * kSlot);
    const uint64_t rev = doc.snapshot().revision;
    OK(!groove_apply(c, swing, 100, report, why) && why.find("Nothing was moved") != std::string::npos);
    OK(doc.snapshot().revision == rev);
    return 0;
}

int test_refusals() {
    ModelDocument doc(session({sixteenths()}));
    EditController c(doc);
    c.set_mode(EditMode::Slip);
    std::string why;
    OK(!groove_command(c, GrooveDeps{}, "groove swing 66 16", why) &&
       why.find("select the regions") != std::string::npos);
    c.select_range(0, kBar, 2 * kBar);
    OK(!groove_command(c, GrooveDeps{}, "groove swing 66 16 0", why) &&
       why.find("strength") != std::string::npos);
    OK(!groove_command(c, GrooveDeps{}, "groove swing 90 16", why) &&
       why.find("50 to 75") != std::string::npos);
    OK(!groove_command(c, GrooveDeps{}, "groove swing 66 12", why) &&
       why.find("pairs") != std::string::npos);
    OK(!groove_command(c, GrooveDeps{}, "groove wobble", why) &&
       why.find("groove swing") != std::string::npos);
    // Straight already: nothing would move, and the operator is told.
    OK(!groove_command(c, GrooveDeps{}, "groove swing 50 16", why) &&
       why.find("already sits") != std::string::npos);
    c.set_mode(EditMode::Shuffle);
    OK(!groove_command(c, GrooveDeps{}, "groove swing 66 16", why) &&
       why.find("Shuffle") != std::string::npos);
    // tempo.edit hands the word over.
    c.set_mode(EditMode::Slip);
    OK(!c.edit_tempo("groove swing 66 12", why) && why.find("pairs") != std::string::npos);
    return 0;
}

int test_saved_templates() {
    const std::string file = scratch_file();
    const GrooveTemplateStore store(file);
    ModelDocument doc(session({sixteenths()}));
    EditController c(doc);
    c.set_mode(EditMode::Slip);
    c.select_range(0, kBar, 2 * kBar);
    GrooveDeps deps;
    deps.store = &store;
    std::string why;

    OK(!groove_command(c, deps, "groove saved Pocket", why) &&
       why.find("no groove is saved") != std::string::npos);
    OK(groove_command(c, deps, "groove save \"Late Pocket\" swing 62 16", why));
    OK(groove_command(c, deps, "groove save Pocket swing 66 16", why));
    std::vector<NamedGroove> all;
    OK(store.load(all, why) && all.size() == 2 && all[0].name == "Late Pocket");
    const auto before = positions(doc, 0);
    OK(groove_command(c, deps, "groove saved Pocket", why));
    OK(near(positions(doc, 0)[1], before[1] + kSlot * 32 / 100));
    OK(c.undo(why));
    OK(groove_command(c, deps, "groove saved \"Late Pocket\" 100", why));
    OK(near(positions(doc, 0)[1], before[1] + kSlot * 24 / 100));
    // Saving under a name replaces it.
    OK(groove_command(c, deps, "groove save Pocket swing 54 16", why));
    OK(store.load(all, why) && all.size() == 2);
    OK(!groove_command(c, deps, "groove saved Missing", why) &&
       why.find("\"Missing\"") != std::string::npos);
    std::error_code ec;
    fs::remove(file, ec);
    return 0;
}

int test_corrupt_file() {
    const std::string file = scratch_file();
    {
        std::ofstream out(file, std::ios::binary);
        out << "{ \"format\": \"sonora.groove-templates\", \"version\": 1, \"templates\": [";
    }
    const GrooveTemplateStore store(file);
    std::vector<NamedGroove> all;
    std::string why;
    OK(!store.load(all, why));
    OK(why.find(file) != std::string::npos && why.find("were not read") != std::string::npos);

    ModelDocument doc(session({sixteenths()}));
    EditController c(doc);
    c.set_mode(EditMode::Slip);
    c.select_range(0, kBar, 2 * kBar);
    GrooveDeps deps;
    deps.store = &store;
    OK(!groove_command(c, deps, "groove saved Pocket", why) &&
       why.find("were not read") != std::string::npos);
    // A save would lose what the file held: refused, the file left as it was.
    OK(!groove_command(c, deps, "groove save Pocket swing 60 16", why));
    std::ifstream in(file, std::ios::binary);
    const std::string kept((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    OK(kept.back() == '[');
    in.close();

    // A newer version is refused by name, not read as empty.
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "{\"format\": \"sonora.groove-templates\", \"version\": 9, \"templates\": []}";
    }
    OK(!store.load(all, why) && why.find("version 9") != std::string::npos);
    // An offset past half a slot, as only a hand edit makes.
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "{\"format\": \"sonora.groove-templates\", \"version\": 1, \"templates\": "
               "[{\"name\": \"x\", \"slots_per_beat\": 2, \"offsets\": [0, 0.9], "
               "\"gains\": [1, 1]}]}";
    }
    OK(!store.load(all, why) && why.find("half a slot") != std::string::npos);
    std::error_code ec;
    fs::remove(file, ec);
    return 0;
}

// Sound over given source-frame spans, silence elsewhere.
class SpanPeaks final : public PeakSource {
public:
    explicit SpanPeaks(std::vector<std::pair<Frames, Frames>> loud, bool ready = true)
        : loud_(std::move(loud)), ready_(ready) {}
    void peaks(Id, uint16_t, Frames first, double fpb, int count, PeakPair* out) const override {
        for (int i = 0; i < count; ++i) {
            const double a = first + i * fpb, b = a + fpb;
            bool hit = false;
            for (const auto& s : loud_) hit = hit || (a < s.second && b > s.first);
            out[i] = hit ? PeakPair{-0.5f, 0.5f} : PeakPair{-0.001f, 0.001f};
        }
    }
    bool ready(Id) const override { return ready_; }

private:
    std::vector<std::pair<Frames, Frames>> loud_;
    bool ready_;
};

int test_peak_onsets() {
    Region r;
    r.id = 10;
    r.name = "Loop";
    r.position = kBar;
    r.length = kBar;
    r.source_offset = 0;
    r.channels = {{100, 0}};
    // Hits 50 ms long at 0, 6240 and 12000 source frames.
    const Frames hit = kRate / 20;
    const SpanPeaks peaks({{0, hit}, {6240, 6240 + hit}, {12000, 12000 + hit}});
    const PeakOnsetSource src(peaks);
    std::vector<Frames> got;
    std::string why;
    OK(src.onsets(r, kRate, got, why));
    OK(got.size() == 3);
    const Frames bucket = kRate / 200;  // 5 ms
    OK(got[0] == kBar);
    OK(got[1] >= kBar + 6240 - bucket && got[1] <= kBar + 6240);
    OK(got[2] == kBar + 12000);

    const SpanPeaks reading({{0, hit}}, false);
    OK(!PeakOnsetSource(reading).onsets(r, kRate, got, why) &&
       why.find("still being read") != std::string::npos);
    // An overview of nothing but zeros has no hit to find, and says so.
    class Zero final : public PeakSource {
        void peaks(Id, uint16_t, Frames, double, int count, PeakPair* out) const override {
            for (int i = 0; i < count; ++i) out[i] = PeakPair{};
        }
    } zero;
    OK(!PeakOnsetSource(zero).onsets(r, kRate, got, why) && why.find("silent") != std::string::npos);
    return 0;
}

// The panel's QML is built in whole, with what the scene and the Edit
// window look for by name.
int test_panel_text() {
    const std::string qml = groove_panel_qml();
    OK(qml.rfind("// SPDX-License-Identifier", 0) == 0);
    OK(qml.find("objectName: \"grooveApply\"") != std::string::npos);
    OK(qml.find("\"tempo.edit\"") != std::string::npos);
    OK(qml.size() > 1000 && qml.back() == '\n');
    return 0;
}

}  // namespace

int main() {
    if (test_swing_and_undo() || test_from_reference() || test_overlap_reported() ||
        test_refusals() || test_saved_templates() || test_corrupt_file() || test_peak_onsets() ||
        test_panel_text()) {
        return 1;
    }
    std::printf("ui groove: ok\n");
    return 0;
}
