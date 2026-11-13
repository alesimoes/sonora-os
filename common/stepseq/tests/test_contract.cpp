// Contract test for common/stepseq (ADR-0169): pattern ranges, sample-exact
// triggers independent of block size, seeded probability, mute/solo, overflow
// counting, the lock-free snapshot exchange, and no allocation in render.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <tuple>
#include <vector>

#include "sonora/stepseq/fake_player.h"
#include "sonora/stepseq/pattern_exchange.h"

using namespace sonora::stepseq;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static long g_allocs = 0;
void* operator new(std::size_t n) {
    ++g_allocs;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

using Hit = std::tuple<std::int64_t, int, std::int64_t>; // absolute frame, row, step

Step on(int vel = 100, int prob = 100, int nudge = 0) {
    Step s;
    s.on = true;
    s.velocity = static_cast<std::uint8_t>(vel);
    s.probability = static_cast<std::uint8_t>(prob);
    s.nudge_ticks = static_cast<std::int16_t>(nudge);
    return s;
}

Row row_of(SourceId id) {
    Row r;
    r.source = id;
    return r;
}

std::vector<Hit> render_all(FakePatternPlayer& p, const Pattern& pat, const TempoSnapshot& t,
                            std::int64_t start, std::int64_t end, int block) {
    std::vector<Hit> hits;
    auto out = std::make_unique<EventBlock>();
    for (std::int64_t f = start; f < end; f += block) {
        const auto n = static_cast<std::int32_t>(end - f < block ? end - f : block);
        p.render_events(pat, f, n, t, *out);
        for (int i = 0; i < out->count; ++i) {
            const TriggerEvent& e = out->events[i];
            CHECK(e.frame_offset >= 0 && e.frame_offset < n);
            if (i) CHECK(out->events[i - 1].frame_offset <= e.frame_offset);
            hits.emplace_back(f + e.frame_offset, e.row, e.step_index);
        }
    }
    return hits;
}

void test_pattern_ranges() {
    Pattern p;
    CHECK(p.length() == 16 && p.steps_per_beat() == 4 && p.swing_percent() == 50);
    p.set_length(0);
    CHECK(p.length() == 1);
    p.set_length(500);
    CHECK(p.length() == kMaxSteps);
    p.set_swing_percent(99);
    CHECK(p.swing_percent() == kMaxSwingPercent);
    p.set_row_count(40);
    CHECK(p.row_count() == kMaxRows);
    Step wild = on(200, 250, 5000);
    wild.gate = 3.0f;
    wild.pitch_semitones = 100;
    p.set_step(2, 3, wild);
    const Step& s = p.step(2, 3);
    CHECK(s.velocity == 127 && s.probability == 100 && s.gate == 1.0f);
    CHECK(s.pitch_semitones == kMaxPitchSemitones);
    CHECK(s.nudge_ticks == p.max_nudge_ticks() && p.max_nudge_ticks() == 120);
}

void test_straight_grid() {
    Pattern p;
    p.set_row(0, row_of(7));
    for (int s = 0; s < 16; s += 4) p.set_step(0, s, on(90));
    FakePatternPlayer pl;
    TempoSnapshot t; // 48 kHz, 120 bpm: 6000 frames per sixteenth
    auto out = std::make_unique<EventBlock>();
    pl.render_events(p, 0, 96000, t, *out);
    CHECK(out->count == 4);
    for (int i = 0; i < out->count && i < 4; ++i) {
        CHECK(out->events[i].frame_offset == i * 24000);
        CHECK(out->events[i].source == 7 && out->events[i].velocity == 90);
        CHECK(out->events[i].gate_frames == 3000);
    }
    pl.render_events(p, 96000, 6000, t, *out); // second pass, step 0 again
    CHECK(out->count == 1 && out->events[0].step_index == 16);
    t.bpm = 0.0;
    pl.render_events(p, 0, 96000, t, *out);
    CHECK(out->count == 0);
}

void test_swing_and_nudge() {
    Pattern p;
    p.set_row(0, row_of(1));
    p.set_swing_percent(75);
    p.set_step(0, 1, on());          // odd step: half a step late
    p.set_step(0, 4, on(100, 100, -60)); // 60 ticks * 25 frames early
    FakePatternPlayer pl;
    TempoSnapshot t;
    t.origin_frame = 10000;
    const auto hits = render_all(pl, p, t, 0, 10000 + 96000, 4096);
    CHECK(hits.size() == 2);
    if (hits.size() == 2) {
        CHECK(std::get<0>(hits[0]) == 10000 + 6000 + 3000);
        CHECK(std::get<0>(hits[1]) == 10000 + 24000 - 1500);
    }
}

Pattern busy_pattern() {
    Pattern p;
    p.set_length(13);
    p.set_swing_percent(62);
    for (int r = 0; r < 4; ++r) {
        p.set_row(r, row_of(100 + r));
        for (int s = 0; s < 13; ++s)
            if ((s + r) % 3 != 0) p.set_step(r, s, on(80, 50 + 10 * r, (s * 37 % 241) - 120));
    }
    return p;
}

void test_block_size_independence_and_seed() {
    const Pattern p = busy_pattern();
    TempoSnapshot t;
    t.bpm = 97.3;
    t.sample_rate = 44100.0;
    t.origin_frame = 333;
    FakePatternPlayer pl;
    pl.reset(42);
    const auto a = render_all(pl, p, t, 0, 400000, 64);
    const auto b = render_all(pl, p, t, 0, 400000, 512);
    const auto c = render_all(pl, p, t, 0, 400000, 1001);
    CHECK(!a.empty());
    // Ties on one frame keep arrival order, which differs across block cuts.
    auto sa = a, sb = b, sc = c;
    std::sort(sa.begin(), sa.end());
    std::sort(sb.begin(), sb.end());
    std::sort(sc.begin(), sc.end());
    CHECK(sa == sb && sa == sc);

    pl.reset(42);
    auto again = render_all(pl, p, t, 0, 400000, 777);
    std::sort(again.begin(), again.end());
    CHECK(again == sa);
    pl.reset(43);
    auto other = render_all(pl, p, t, 0, 400000, 777);
    std::sort(other.begin(), other.end());
    CHECK(other != sa);

    int fired = 0, draws = 0;
    for (std::int64_t n = 0; n < 2000; ++n, ++draws) fired += FakePatternPlayer::fires(9, n, 0, 50);
    CHECK(fired > draws * 4 / 10 && fired < draws * 6 / 10);
    CHECK(!FakePatternPlayer::fires(9, 5, 0, 0) && FakePatternPlayer::fires(9, 5, 0, 100));
}

void test_mute_solo() {
    Pattern p;
    for (int r = 0; r < 3; ++r) {
        p.set_row(r, row_of(1 + r));
        p.set_step(r, 0, on());
    }
    Row muted = p.row(0);
    muted.mute = true;
    p.set_row(0, muted);
    FakePatternPlayer pl;
    TempoSnapshot t;
    auto out = std::make_unique<EventBlock>();
    pl.render_events(p, 0, 100, t, *out);
    CHECK(out->count == 2);
    Row solo = p.row(2);
    solo.solo = true;
    p.set_row(2, solo);
    pl.render_events(p, 0, 100, t, *out);
    CHECK(out->count == 1 && out->events[0].row == 2);
    p.set_row(2, Row{}); // no source: triggers nothing
    pl.render_events(p, 0, 100, t, *out);
    CHECK(out->count == 1 && out->events[0].row == 1);
}

void test_overflow_and_no_allocation() {
    Pattern p;
    p.set_length(64);
    p.set_row_count(16);
    p.set_steps_per_beat(8);
    for (int r = 0; r < 16; ++r) {
        p.set_row(r, row_of(1 + r));
        for (int s = 0; s < 64; ++s) p.set_step(r, s, on(100, 70));
    }
    FakePatternPlayer pl;
    pl.reset(1);
    TempoSnapshot t;
    t.bpm = 300.0;
    auto out = std::make_unique<EventBlock>();
    const long before = g_allocs;
    pl.render_events(p, 0, 48000 * 4, t, *out);
    CHECK(g_allocs == before);
    CHECK(out->count == kMaxEventsPerBlock && out->dropped > 0);
}

// A nudge stored at a coarse grid must not outgrow half a step when the grid
// gets finer, or the player's scan window misses it and the hit depends on
// how the timeline is cut into blocks.
void test_nudge_reclamped_on_grid_change() {
    Pattern p;
    p.set_row(0, row_of(1));
    p.set_steps_per_beat(1);
    p.set_step(0, 8, on(100, 100, -480));
    CHECK(p.step(0, 8).nudge_ticks == -480);
    p.set_steps_per_beat(8);
    CHECK(p.step(0, 8).nudge_ticks >= -p.max_nudge_ticks());
    CHECK(p.step(0, 8).nudge_ticks <= p.max_nudge_ticks());

    FakePatternPlayer a, b;
    a.reset(3);
    b.reset(3);
    TempoSnapshot t;
    const auto whole = render_all(a, p, t, 0, 96000, 96000);
    const auto small = render_all(b, p, t, 0, 96000, 64);
    CHECK(!whole.empty());
    CHECK(whole == small);
}

void test_exchange() {
    auto x = std::make_unique<PatternExchange>();
    Pattern a, b, c;
    a.set_length(5);
    b.set_length(6);
    c.set_length(7);
    CHECK(x->publish(a));
    const long before = g_allocs;
    CHECK(x->acquire().length() == 5); // audio now holds a's slot
    CHECK(g_allocs == before);
    CHECK(x->publish(b));             // the other slot is free
    CHECK(!x->publish(c));            // the spare is a's slot, still held
    CHECK(x->acquire().length() == 6);
    CHECK(x->publish(c));
    CHECK(x->acquire().length() == 7);
}

} // namespace

int main() {
    test_pattern_ranges();
    test_straight_grid();
    test_swing_and_nudge();
    test_block_size_independence_and_seed();
    test_mute_solo();
    test_overflow_and_no_allocation();
    test_nudge_reclamped_on_grid_change();
    test_exchange();
    std::printf("stepseq contract: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
