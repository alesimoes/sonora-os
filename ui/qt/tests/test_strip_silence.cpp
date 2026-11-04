// SPDX-License-Identifier: GPL-3.0-or-later
//
// Strip Silence: where the quiet stretches are found, what the edit makes of
// them (separations and removals, one undo step), and what it refuses.
#include <QGuiApplication>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "edit_controller.h"
#include "strip_silence.h"
#include "timeline_document.h"

using namespace sonora;
using namespace sonora::ui;
using namespace sonora::ui::model;

#define OK(cond)                                                              \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                         \
        }                                                                     \
    } while (0)

namespace {

constexpr uint32_t kRate = 48000;
constexpr Frames kSec = kRate;

// Sound (0.5) over given source-frame spans, silence elsewhere.
class SpanPeaks final : public PeakSource {
public:
    explicit SpanPeaks(std::vector<std::pair<Frames, Frames>> loud, float level = 0.5f)
        : loud_(std::move(loud)), level_(level) {}
    void peaks(Id, uint16_t, Frames first, double fpb, int count, PeakPair* out) const override {
        for (int i = 0; i < count; ++i) {
            const double a = first + i * fpb, b = a + fpb;
            bool hit = false;
            for (const auto& s : loud_) hit = hit || (a < s.second && b > s.first);
            out[i] = hit ? PeakPair{-level_, level_} : PeakPair{-0.0001f, 0.0001f};
        }
    }

private:
    std::vector<std::pair<Frames, Frames>> loud_;
    float level_;
};

// One track, region 10 at 2 s, 4 s long, playing the source from 2 s.
Snapshot session() {
    Snapshot s = make_empty_session(1, kRate);
    auto pool = std::make_shared<SourcePool>();
    Source take;
    take.id = 100;
    take.path = "Audio Files/Take 1.wav";
    take.length = 60 * kSec;
    pool->sources.push_back(take);
    s.sources = pool;
    s.next_id = 101;
    auto edit = std::make_shared<EditState>(*s.edit);
    auto t = std::make_shared<TrackEdit>(*edit->tracks[0]);
    auto p = std::make_shared<Playlist>(*t->playlists[0]);
    Region a;
    a.id = 10;
    a.name = "A";
    a.position = 2 * kSec;
    a.length = 4 * kSec;
    a.source_offset = 2 * kSec;
    a.channels = {{100, 0}};
    p->regions = {a};
    t->playlists[0] = p;
    edit->tracks[0] = t;
    s.edit = edit;
    return s;
}

std::vector<Region> regions(const TimelineDocument& d) {
    return active_playlist(d.snapshot().edit->tracks[0].get())->regions;
}

// Sound at region seconds 0.5-1 and 3-3.5 (source 2.5-3 s, 5-5.5 s).
SpanPeaks two_phrases() {
    return SpanPeaks({{2 * kSec + kSec / 2, 3 * kSec}, {5 * kSec, 5 * kSec + kSec / 2}});
}

int test_parse() {
    StripParams p;
    std::string why;
    OK(parse_strip_params("", p, why) && p.threshold_db == -40.0);
    OK(parse_strip_params("-50,200,5,15", p, why));
    OK(p.threshold_db == -50.0 && p.min_silence_ms == 200.0 && p.start_pad_ms == 5.0 &&
       p.end_pad_ms == 15.0);
    OK(!parse_strip_params("loud", p, why) && why.find("not a number") != std::string::npos);
    OK(!parse_strip_params("6", p, why) && why.find("-120 and 0") != std::string::npos);
    OK(!parse_strip_params("-40,0", p, why));
    OK(!parse_strip_params("-40,100,1,1,1", p, why));
    OK(p.threshold_db == -50.0);  // a refused string leaves the output alone
    return 0;
}

int test_find() {
    const Snapshot s = session();
    const Region& r = *find_region(s, 10);
    const SpanPeaks peaks = two_phrases();
    const auto spans = find_silence(peaks, r, kRate, StripParams{});
    OK(spans.size() == 3);
    // 10 ms pads: leading to 0.49 s, the gap 1.01-2.99, the tail from 3.51.
    OK(spans[0].from == 2 * kSec && spans[0].to == 2 * kSec + kSec / 2 - 10 * kRate / 1000);
    OK(spans[1].from == 3 * kSec + 10 * kRate / 1000 && spans[1].to == 5 * kSec - 10 * kRate / 1000);
    OK(spans[2].from == 5 * kSec + kSec / 2 + 10 * kRate / 1000 && spans[2].to == 6 * kSec);

    // A pause shorter than the minimum stays.
    StripParams longer;
    longer.min_silence_ms = 1800;
    const auto few = find_silence(peaks, r, kRate, longer);
    OK(few.size() == 1 && few[0].from == 3 * kSec + 10 * kRate / 1000);
    return 0;
}

int test_edit() {
    ModelDocument doc(session());
    EditController c(doc);
    c.set_mode(EditMode::Slip);
    const SpanPeaks peaks = two_phrases();
    std::string why;

    // Nothing selected.
    OK(!c.strip_silence(peaks, StripParams{}, why) && why.find("select a region") != std::string::npos);

    OK(c.select_region(10));
    const uint64_t before = doc.snapshot().revision;
    OK(c.strip_silence(peaks, StripParams{}, why));
    OK(doc.snapshot().revision == before + 1);  // one step, however many cuts
    const auto kept = regions(doc);
    OK(kept.size() == 2);
    OK(kept[0].position == 2 * kSec + kSec / 2 - 10 * kRate / 1000);
    OK(kept[0].end() == 3 * kSec + 10 * kRate / 1000);
    OK(kept[1].position == 5 * kSec - 10 * kRate / 1000);
    OK(kept[1].end() == 5 * kSec + kSec / 2 + 10 * kRate / 1000);
    // What stays still plays the same source frames at the same times.
    OK(kept[0].source_offset == kept[0].position);
    OK(kept[1].source_offset == kept[1].position);
    std::string error;
    OK(check_invariants(doc.snapshot(), error));

    OK(doc.undo(why));
    OK(regions(doc).size() == 1 && regions(doc)[0].length == 4 * kSec);
    OK(doc.redo(why) && regions(doc).size() == 2);
    return 0;
}

int test_refusals() {
    ModelDocument doc(session());
    EditController c(doc);
    c.set_mode(EditMode::Slip);
    std::string why;
    OK(c.select_region(10));

    const SpanPeaks steady({{0, 60 * kSec}});
    OK(!c.strip_silence(steady, StripParams{}, why) && why.find("-40 dB for 100 ms") != std::string::npos);
    OK(regions(doc).size() == 1);

    const SpanPeaks nothing({});
    OK(!c.strip_silence(nothing, StripParams{}, why) && why.find("everything selected") != std::string::npos);
    OK(regions(doc).size() == 1);

    // A level under the threshold is silence: the same sound, a higher threshold.
    StripParams high;
    high.threshold_db = -3.0;  // 0.5 is -6 dB
    OK(!c.strip_silence(steady, high, why) && why.find("everything selected") != std::string::npos);

    c.set_mode(EditMode::Shuffle);
    OK(!c.strip_silence(two_phrases(), StripParams{}, why) && why.find("Shuffle") != std::string::npos);
    OK(regions(doc).size() == 1);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (test_parse() || test_find() || test_edit() || test_refusals()) return 1;
    std::printf("strip silence: ok\n");
    return 0;
}
