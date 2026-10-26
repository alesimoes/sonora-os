// SPDX-License-Identifier: GPL-3.0-or-later
//
// The click against the tempo map (ADR-0020 §1): every beat the map counts
// sounds on the frame model::MusicalMap gives it, across tempo changes that
// carry the bar on and meter changes that cut one short, accented where the
// map reads beat 1; the segments the builder makes agree with the map hours
// in, on the 128-bit path. The count-off takes its bars from the document,
// and the two toggles are commands.
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "sonora/click.h"
#include "sonora/command.h"
#include "sonora/model/history.h"
#include "sonora/model/musical_map.h"
#include "sonora/model/operations.h"
#include "sonora/plan_builder.h"
#include "sonora/playback_controller.h"
#include "sonora/session.h"
#include "sonora/session_commands.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

namespace fs = std::filesystem;
using namespace sonora::model;
using sonora::ClickSegment;
using sonora::PlaybackController;
using sonora::Session;

constexpr uint32_t kBlock = 64;
constexpr int64_t kRolled = 900000;

struct Onset {
    int64_t frame;
    bool accent;
    bool operator==(const Onset& o) const { return frame == o.frame && accent == o.accent; }
};

// Four entries: 4/4 at 120; at 100000, mid-beat, 97.3 bpm, a bpm whose
// exact value needs the conversion's slow path, carrying the bar on; at
// 300007, mid-bar, 7/8 at 133.7, which cuts that bar short; at 500000 3/4
// at 61.25.
SessionSettings changing_map() {
    SessionSettings s;
    s.tempo = {TempoEntry{0, 120.0f, {{4, 4}}}, TempoEntry{100000, 97.3f, {{4, 4}}},
               TempoEntry{300007, 133.7f, {{7, 8}}}, TempoEntry{500000, 61.25f, {{3, 4}}}};
    return s;
}

// Every beat the map counts before `until`, read through its own reading:
// beat b of bar n is a beat when bars_beats_at() reads its tick back as
// n|b|000.
bool beats_of(const MusicalMap& map, int64_t until, std::vector<Onset>& out) {
    for (int64_t bar = 1;; ++bar) {
        for (int64_t beat = 1;; ++beat) {
            Ticks t = 0;
            if (!map.tick_of(BarsBeats{bar, beat, 0}, t)) return false;
            if (!(map.bars_beats_at(t) == BarsBeats{bar, beat, 0})) {
                if (beat == 1) return false;
                break;
            }
            Frames f = 0;
            if (!map.frame_at(t, f)) return false;
            if (f >= until) return true;
            out.push_back({f, beat == 1});
        }
    }
}

int test_beats_follow_the_tempo_map() {
    const fs::path dir = fs::temp_directory_path() / "sonora_test_click_control";
    fs::remove_all(dir);
    sonora::SessionConfig cfg;
    cfg.dir = dir.string();
    cfg.track_count = 1;
    cfg.playback.chunk_frames = 256;
    cfg.playback.window_seconds = 0.02;
    cfg.playback.pool_chunks = 64;
    cfg.playback.max_block_frames = 256;
    Session s(cfg);
    s.create_dirs();
    History h(new_session(changing_map()));
    PlaybackController ctl(s);
    std::string error;
    CHECK(ctl.show(h.current(), error));
    CHECK(ctl.settle(2.0));
    s.click().set_on(true);
    ctl.play();
    CHECK(ctl.settle(2.0));

    std::vector<float> l(kBlock), r(kBlock);
    float* outs[2] = {l.data(), r.data()};
    sonora::backend::AudioBlock b{};
    b.outputs = outs;
    b.output_count = 2;
    b.nframes = kBlock;
    std::vector<Onset> got;
    for (int64_t at = 0; at < kRolled; at += kBlock) {
        CHECK(static_cast<int64_t>(s.transport().playhead()) == at);
        s.process(b);
        for (uint32_t i = 0; i < kBlock; ++i) {
            if (l[i] == s.click().accent_peak()) got.push_back({at + i, true});
            if (l[i] == s.click().beat_peak()) got.push_back({at + i, false});
        }
    }
    MusicalMap map;
    CHECK(map.build(changing_map(), error));
    std::vector<Onset> want;
    CHECK(beats_of(map, kRolled, want));
    CHECK(want.size() > 30);
    if (got != want) {
        std::fprintf(stderr, "  %zu onsets, the map has %zu\n", got.size(), want.size());
        for (std::size_t i = 0; i < got.size() && i < want.size(); ++i) {
            if (got[i] == want[i]) continue;
            std::fprintf(stderr, "  [%zu] %lld%s, the map %lld%s\n", i,
                         static_cast<long long>(got[i].frame), got[i].accent ? " accent" : "",
                         static_cast<long long>(want[i].frame), want[i].accent ? " accent" : "");
            break;
        }
        CHECK(false);
    }
    s.stop();
    std::error_code ec;
    fs::remove_all(dir, ec);
    return 0;
}

// Far from the entries, where only the 128-bit conversion is exact: beat j
// of each segment is on the map's grid, at the map's frame, and accented
// where the map reads beat 1.
int test_segments_agree_with_the_map_far_out() {
    const SessionSettings settings = changing_map();
    MusicalMap map;
    std::string error;
    CHECK(map.build(settings, error));
    const std::vector<ClickSegment> segs = sonora::click_segments(settings);
    CHECK(segs.size() == map.entries().size());
    int64_t last_frame = -1;
    for (std::size_t k = 0; k < segs.size(); ++k) {
        const ClickSegment& c = segs[k];
        const MusicalMap::Entry& e = map.entries()[k];
        CHECK(c.beats > 0);
        CHECK(c.first_frame > last_frame);
        last_frame = c.first_frame;
        std::vector<uint64_t> js = {0, 1, 2, c.beats / 2, c.beats - 1};
        if (k + 1 == segs.size()) js = {0, 1, 1000, 1000003, 123456789, 987654321};
        for (const uint64_t j : js) {
            const Ticks t = e.tick + static_cast<Ticks>(c.first_tick + j * c.beat_ticks);
            Frames f = 0;
            CHECK(map.frame_at(t, f));
            CHECK(sonora::click_beat_frame(c, j) == f);
            const BarsBeats bb = map.bars_beats_at(t);
            CHECK(bb.tick == 0);
            CHECK(((c.first_beat + j) % c.beats_per_bar == 0) == (bb.beat == 1));
            if (k + 1 < segs.size()) CHECK(t < map.entries()[k + 1].tick);
        }
    }
    // A map that does not build clicks nothing rather than something wrong.
    SessionSettings bad = settings;
    bad.tempo[1].bpm = 0.0f;
    CHECK(sonora::click_segments(bad).empty());
    return 0;
}

// transport.count_off_bars reaches the session with every snapshot shown,
// and the toggles flip the click and the count-off.
int test_document_bars_and_toggles() {
    const fs::path dir = fs::temp_directory_path() / "sonora_test_click_bars";
    fs::remove_all(dir);
    sonora::SessionConfig cfg;
    cfg.dir = dir.string();
    cfg.track_count = 1;
    Session s(cfg);
    s.create_dirs();
    CHECK(s.click().count_off_bars() == 2);
    SessionSettings settings;
    settings.transport.count_off_bars = 3;
    History h(new_session(settings));
    PlaybackController ctl(s);
    std::string error;
    CHECK(ctl.show(h.current(), error));
    CHECK(s.click().count_off_bars() == 3);
    SetSettings set{h.current().settings};
    set.settings.transport.count_off_bars = 1;
    CHECK(h.apply(set, error));
    CHECK(ctl.show(h.current(), error));
    CHECK(s.click().count_off_bars() == 1);
    set.settings.transport.count_off_bars = 0;
    CHECK(!h.apply(set, error));
    CHECK(error.find("count_off_bars") != std::string::npos);

    sonora::CommandRegistry reg;
    int take = 1;
    bool quit = false;
    sonora::install_session_commands(reg, s, take, quit);
    CHECK(!s.click().on() && !s.click().count_off());
    CHECK(reg.dispatch("transport.toggle_click") == sonora::DispatchResult::Ok);
    CHECK(s.click().on());
    CHECK(reg.dispatch("transport.toggle_count_off") == sonora::DispatchResult::Ok);
    CHECK(s.click().count_off());
    CHECK(reg.dispatch("transport.toggle_click") == sonora::DispatchResult::Ok);
    CHECK(reg.dispatch("transport.toggle_count_off") == sonora::DispatchResult::Ok);
    CHECK(!s.click().on() && !s.click().count_off());
    std::error_code ec;
    fs::remove_all(dir, ec);
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    const struct {
        const char* name;
        int (*fn)();
    } tests[] = {
        {"beats_follow_the_tempo_map", test_beats_follow_the_tempo_map},
        {"segments_agree_with_the_map_far_out", test_segments_agree_with_the_map_far_out},
        {"document_bars_and_toggles", test_document_bars_and_toggles},
    };
    for (const auto& t : tests) {
        const int r = t.fn();
        std::printf("%s %s\n", r == 0 ? "PASS" : "FAIL", t.name);
        failed += r;
    }
    return failed == 0 ? 0 : 1;
}
