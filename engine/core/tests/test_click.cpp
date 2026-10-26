// SPDX-License-Identifier: GPL-3.0-or-later
//
// The click and the count-off on the audio thread (ADR-0020): beats on the
// frame a segment gives them, accented on the downbeat, through a locate
// and a playback loop; a count-off of the right length that holds the take
// and the playhead, ends on a block boundary and is cancelled by a stop;
// nothing of either in the take; and no heap touched by process(). The
// beats against the tempo map itself are engine/control's test_click.
#include <cstdio>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

#define PLAYBACK_TEST_COUNT_HEAP 1
#include "playback_test.h"
#include "sonora/click.h"
#include "sonora/session.h"
#include "sonora/wav_reader.h"

namespace {

namespace fs = std::filesystem;
using playback_test::heap_untouched;
using playback_test::reset_heap_counts;
using sonora::ClickSegment;
using sonora::PlaybackPlan;
using sonora::Session;
using sonora::TransportState;

constexpr uint32_t kBlock = 64;
constexpr uint32_t kQuarter = 960000;   // model::kTicksPerQuarter

struct Rig {
    fs::path dir;
    std::unique_ptr<Session> session;

    explicit Rig(const std::string& name) {
        dir = fs::temp_directory_path() / ("sonora_test_click_" + name);
        fs::remove_all(dir);
        sonora::SessionConfig cfg;
        cfg.dir = dir.string();
        cfg.track_count = 1;
        cfg.format = sonora::WavWriter::Format::Float32;
        cfg.ring_seconds = 1.0;
        cfg.playback.chunk_frames = 256;
        cfg.playback.window_seconds = 0.02;
        cfg.playback.pool_chunks = 64;
        cfg.playback.max_block_frames = 256;
        session = std::make_unique<Session>(cfg);
        session->create_dirs();
        // Nothing but the click reaches the outputs.
        session->tracks()[0]->set_monitor(sonora::Track::Monitor::Off);
    }
    ~Rig() {
        session.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

// 4/4 at `bpm` from frame 0 at 48 kHz, every beat to the end of the
// timeline: ticks per frame bpm * 960000 / (60 * 48000) = bpm / 3.
ClickSegment four_four(uint64_t bpm, int64_t at = 0, uint32_t beats_per_bar = 4) {
    ClickSegment c;
    c.at = at;
    c.first_frame = at;
    c.beat_ticks = kQuarter;
    c.num = bpm;
    c.den = 3;
    c.beats = uint64_t{1} << 30;
    c.beats_per_bar = beats_per_bar;
    return c;
}

bool publish_click(Session& s, const std::vector<ClickSegment>& segs) {
    sonora::PlanSizes sizes;
    sizes.tracks = 1;
    sizes.nodes = 3;
    sizes.edges = 2;
    sizes.click_segments = static_cast<uint32_t>(segs.size());
    PlaybackPlan* p = sonora::create_plan(sizes);
    p->epoch = s.plans().next_epoch();
    p->chunk_frames = 256;
    p->pan_exponent = 0.5f;
    p->tracks[0].channels = 1;
    sonora::layout_monitor_graph(*p);
    for (std::size_t i = 0; i < segs.size(); ++i) p->click[i] = segs[i];
    return playback_test::publish_plan(s, p);
}

// One block of process() with the heap counted, the input `in` on track 0
// (null: unconnected), the outputs into `l` and `r`.
void run(Session& s, std::vector<float>& l, std::vector<float>& r,
         const float* in = nullptr) {
    l.assign(kBlock, -1.0f);
    r.assign(kBlock, -1.0f);
    float* outs[2] = {l.data(), r.data()};
    const float* ins[1] = {in};
    sonora::backend::AudioBlock b{};
    b.inputs = ins;
    b.input_count = 1;
    b.outputs = outs;
    b.output_count = 2;
    b.nframes = kBlock;
    playback_test::t_counting = true;
    s.process(b);
    playback_test::t_counting = false;
}

struct Onset {
    uint64_t frame;
    bool accent;
    bool operator==(const Onset& o) const { return frame == o.frame && accent == o.accent; }
};

// Where a click starts in `out`, which holds nothing else, `base` being
// its first frame: a sound's first sample is its loudest.
void onsets(const sonora::Click& c, const std::vector<float>& out, uint64_t base,
            std::vector<Onset>& found) {
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == c.accent_peak()) found.push_back({base + i, true});
        if (out[i] == c.beat_peak()) found.push_back({base + i, false});
    }
}

bool same(const std::vector<Onset>& got, const std::vector<Onset>& want) {
    if (got == want) return true;
    std::fprintf(stderr, "  %zu onsets, expected %zu\n", got.size(), want.size());
    for (std::size_t i = 0; i < got.size() || i < want.size(); ++i) {
        const bool g = i < got.size();
        const bool w = i < want.size();
        if (g && w && got[i] == want[i]) continue;
        std::fprintf(stderr, "  [%zu] got %lld%s, expected %lld%s\n", i,
                     g ? static_cast<long long>(got[i].frame) : -1LL,
                     g && got[i].accent ? " accent" : "",
                     w ? static_cast<long long>(want[i].frame) : -1LL,
                     w && want[i].accent ? " accent" : "");
        break;
    }
    return false;
}

// round(x * den / num) in 64 steps against the 128-bit product, halves up.
int test_scale_is_the_exact_rounding() {
#if defined(__SIZEOF_INT128__)
    std::mt19937_64 rng(20261025);
    for (int i = 0; i < 200000; ++i) {
        const uint64_t num = 1 + (rng() >> (11 + rng() % 40));
        const uint64_t den = 1 + (rng() >> (11 + rng() % 40));
        const uint64_t x = rng() >> (11 + rng() % 50);
        const unsigned __int128 p = static_cast<unsigned __int128>(x) * den;
        const unsigned __int128 want = (2 * p + num) / (2 * static_cast<unsigned __int128>(num));
        const int64_t got = sonora::click_scale(x, den, num);
        if (want > (static_cast<unsigned __int128>(1) << 62)) {
            CHECK(got == sonora::kNoClickFrame);
        } else if (static_cast<unsigned __int128>(got) != want) {
            std::fprintf(stderr, "  x %llu den %llu num %llu\n", static_cast<unsigned long long>(x),
                         static_cast<unsigned long long>(den), static_cast<unsigned long long>(num));
            CHECK(false);
        }
    }
#endif
    CHECK(sonora::click_scale(1, 1, 2) == 1);   // a half rounds up
    CHECK(sonora::click_scale(1, 1, 3) == 0);
    CHECK(sonora::click_scale(5, 1, 0) == sonora::kNoClickFrame);
    return 0;
}

// 120 bpm in 4/4 (24000 frames a beat) and, from frame 100000, 3/4 in a
// segment of its own: each beat on its frame, the first of each bar
// accented, the click off sounding nothing and stopped sounding nothing.
int test_beats_on_their_frames() {
    Rig rig("beats");
    Session& s = *rig.session;
    ClickSegment first = four_four(120);
    first.beats = 5;   // 0, 24000, ..., 96000
    ClickSegment second = four_four(120, 100000, 3);
    // Beats from 100000 on in thirds of a quarter, so not on a frame
    // multiple of the first's: 100000 + round(j * 8000).
    second.beat_ticks = kQuarter / 3;
    CHECK(publish_click(s, {first, second}));
    sonora::Click& c = s.click();
    std::vector<float> l, r;
    std::vector<Onset> got;

    s.play();
    run(s, l, r);
    CHECK(l == std::vector<float>(kBlock, 0.0f));   // the click is off
    reset_heap_counts();
    c.set_on(true);
    s.transport().set_state(TransportState::Stopped);
    s.post_locate(0);
    run(s, l, r);
    onsets(c, l, 0, got);
    CHECK(got.empty());   // stopped: no click
    s.play();
    for (uint64_t at = 0; at < 160000; at += kBlock) {
        run(s, l, r);
        CHECK(l == r);
        onsets(c, l, at, got);
    }
    std::vector<Onset> want;
    for (uint64_t j = 0; j < 5; ++j) want.push_back({j * 24000, j % 4 == 0});
    for (uint64_t j = 0; 100000 + j * 8000 < 160000; ++j) want.push_back({100000 + j * 8000, j % 3 == 0});
    CHECK(same(got, want));
    CHECK(heap_untouched());
    return 0;
}

// A locate deep into a segment finds the next beat by search, and a
// playback loop that wraps inside a block clicks at A's beats again.
int test_locate_and_loop() {
    Rig rig("locate_loop");
    Session& s = *rig.session;
    CHECK(publish_click(s, {four_four(120)}));
    sonora::Click& c = s.click();
    c.set_on(true);
    std::vector<float> l, r;
    std::vector<Onset> got;
    reset_heap_counts();

    // 172800000 is 2 hours; the next beat after it plus 10 is 2 h + 24000.
    const uint64_t far = 172800010;
    CHECK(s.post_locate(far));
    s.play();
    for (uint64_t at = far; at < far + 30000; at += kBlock) {
        run(s, l, r);
        onsets(c, l, at, got);
    }
    CHECK(same(got, {{172800000 + 24000, false}}));   // beat 7201, 7201 % 4 == 1

    // Loop [12000, 60000): from 40000, beat 48000, then the wrap at 60000
    // to 12000 sounds 24000 and 48000 again.
    s.transport().set_state(TransportState::Stopped);
    run(s, l, r);
    std::string error;
    CHECK(s.set_loop(12000, 60000, &error));
    s.reader().run_pass();
    s.pump_loop();
    CHECK(s.post_locate(40000));
    run(s, l, r);
    got.clear();
    s.play();
    for (uint64_t n = 0; n < 2000; ++n) {
        const uint64_t at = s.transport().playhead();
        run(s, l, r);
        for (std::size_t i = 0; i < l.size(); ++i) {
            const uint64_t t = at + i < 60000 ? at + i : 12000 + (at + i - 60000) % 48000;
            if (l[i] == c.accent_peak()) got.push_back({t, true});
            if (l[i] == c.beat_peak()) got.push_back({t, false});
        }
        if (got.size() >= 4) break;
    }
    CHECK(same(got, {{48000, false}, {24000, false}, {48000, false}, {24000, false}}));
    CHECK(heap_untouched());
    return 0;
}

// 110 bpm, 1 bar: 4 beats of 26181.8 frames, 104727 frames, 23 past a
// 64-frame block, so the count-off starts 41 frames into its first block
// and ends on the 1637th's end. The take and the playhead wait for it; the
// first recorded frame is the input of the next block, at the take's
// origin, and neither the count-off nor the click is in the file.
int test_count_off_holds_the_take() {
    Rig rig("count_off");
    Session& s = *rig.session;
    CHECK(publish_click(s, {four_four(110)}));
    sonora::Click& c = s.click();
    c.set_on(true);
    c.set_count_off(true);
    c.set_count_off_bars(1);
    s.tracks()[0]->set_rec_armed(true);
    s.disk().start();
    CHECK(s.post_locate(5000));
    std::vector<float> l, r;
    run(s, l, r);
    CHECK(s.transport().playhead() == 5000);

    std::string error;
    CHECK(s.start_recording(1, &error));
    reset_heap_counts();
    std::vector<float> in(kBlock);
    std::vector<Onset> got;
    const uint64_t blocks = (41 + 104727) / kBlock;
    CHECK(blocks == 1637);
    for (uint64_t b = 0; b < blocks; ++b) {
        for (uint32_t i = 0; i < kBlock; ++i) in[i] = -0.25f;   // never recorded
        run(s, l, r, in.data());
        onsets(c, l, b * kBlock, got);
        CHECK(c.counting_off() == (b + 1 < blocks));
        CHECK(s.transport().playhead() == 5000);
        CHECK(s.transport().state() == TransportState::Recording);
    }
    CHECK(same(got, {{41, true}, {41 + 26182, false}, {41 + 52364, false}, {41 + 78545, false}}));
    CHECK(s.take_origin() == Session::kNoOrigin);

    // The take: 600 blocks from 5000, past the first beat at 26182.
    got.clear();
    for (uint64_t b = 0; b < 600; ++b) {
        for (uint32_t i = 0; i < kBlock; ++i) in[i] = static_cast<float>(b * kBlock + i) / 65536.0f;
        run(s, l, r, in.data());
        onsets(c, l, 5000 + b * kBlock, got);
        CHECK(!c.counting_off());
    }
    CHECK(s.transport().playhead() == 5000 + 600 * kBlock);
    CHECK(same(got, {{26182, false}}));
    CHECK(heap_untouched());
    CHECK(s.stop(&error));
    CHECK(s.take_origin() == 5000);

    sonora::WavReader w;
    CHECK(w.open(s.tracks()[0]->writer().path(), error));
    CHECK(w.time_reference() == 5000);
    CHECK(w.frames() == 600 * kBlock);
    std::vector<float> take(static_cast<std::size_t>(w.frames()));
    CHECK(w.read(0, w.frames(), take.data()) == w.frames());
    for (std::size_t f = 0; f < take.size(); ++f) {
        if (take[f] != static_cast<float>(f) / 65536.0f) {
            std::fprintf(stderr, "  take frame %zu: %f\n", f, take[f]);
            CHECK(false);
        }
    }
    return 0;
}

// Stopped during the count-off, the next roll starts at once; a take
// started rolling (on the fly) never counts off; with count-off off, a
// take records from its first block.
int test_count_off_only_from_stop() {
    Rig rig("count_off_cancel");
    Session& s = *rig.session;
    CHECK(publish_click(s, {four_four(120)}));
    sonora::Click& c = s.click();
    c.set_count_off(true);
    s.tracks()[0]->set_rec_armed(true);
    s.disk().start();
    std::vector<float> l, r;
    std::string error;
    reset_heap_counts();

    CHECK(s.start_recording(1, &error));
    for (int b = 0; b < 10; ++b) run(s, l, r);
    CHECK(c.counting_off() && s.transport().playhead() == 0);
    CHECK(s.stop(&error));
    s.disk().start();
    run(s, l, r);
    CHECK(!c.counting_off());
    s.play();
    run(s, l, r);
    CHECK(s.transport().playhead() == kBlock);

    // Rolling: on the fly, at once.
    CHECK(s.start_recording(2, &error));
    run(s, l, r);
    CHECK(!c.counting_off());
    CHECK(s.transport().playhead() == 2 * kBlock);
    CHECK(s.take_origin() == kBlock);
    CHECK(s.stop(&error));
    s.disk().start();

    c.set_count_off(false);
    CHECK(s.start_recording(3, &error));
    run(s, l, r);
    CHECK(!c.counting_off());
    CHECK(s.take_origin() == 2 * kBlock);
    CHECK(s.stop(&error));
    CHECK(heap_untouched());
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    const struct {
        const char* name;
        int (*fn)();
    } tests[] = {
        {"scale_is_the_exact_rounding", test_scale_is_the_exact_rounding},
        {"beats_on_their_frames", test_beats_on_their_frames},
        {"locate_and_loop", test_locate_and_loop},
        {"count_off_holds_the_take", test_count_off_holds_the_take},
        {"count_off_only_from_stop", test_count_off_only_from_stop},
    };
    for (const auto& t : tests) {
        const int r = t.fn();
        std::printf("%s %s\n", r == 0 ? "PASS" : "FAIL", t.name);
        failed += r;
    }
    return failed == 0 ? 0 : 1;
}
