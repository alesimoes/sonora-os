// SPDX-License-Identifier: GPL-3.0-or-later
//
// MusicalMap (brain/specs/midi.md §2): exact frames <-> ticks against values
// computed independently with exact fractions, the three properties §2.2
// lists, the bounds, refusal instead of wrap, negative rounding, the batch
// API, and bars|beats as session-format.md counts them.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "model_test.h"
#include "sonora/model/musical_map.h"

using namespace model_test;

namespace {

MusicalMap map_of(std::uint32_t rate, std::vector<TempoEntry> tempo) {
    MusicalMap m;
    std::string why;
    if (!m.build(rate, tempo, why)) std::fprintf(stderr, "  build: %s\n", why.c_str());
    return m;
}

Ticks tick(const MusicalMap& m, Frames f) {
    Ticks t = -1;
    if (!m.tick_at(f, t)) std::fprintf(stderr, "  tick_at(%lld) refused\n", static_cast<long long>(f));
    return t;
}

Frames frame(const MusicalMap& m, Ticks t) {
    Frames f = -1;
    if (!m.frame_at(t, f)) std::fprintf(stderr, "  frame_at(%lld) refused\n", static_cast<long long>(t));
    return f;
}

bool refuses(const std::vector<TempoEntry>& tempo, std::uint32_t rate, const char* needle) {
    MusicalMap m;
    std::string why;
    if (m.build(rate, tempo, why)) {
        std::fprintf(stderr, "  built, expected \"%s\"\n", needle);
        return false;
    }
    if (!mentions(why, needle)) {
        std::fprintf(stderr, "  expected \"%s\" in: %s\n", needle, why.c_str());
        return false;
    }
    return m.empty();
}

// The golden values come from Python's fractions.Fraction on each float's
// exact value, not from this code: a 44.1 kHz map whose tempi are not
// representable in binary, so every product takes the 128-bit path.
int test_exact_values() {
    const MusicalMap m = map_of(44100, {TempoEntry{0, 97.3f, {{4, 4}}},
                                        TempoEntry{1000003, 133.7f, {{4, 4}}},
                                        TempoEntry{2500000, 61.1f, {{4, 4}}}});
    CHECK(m.reachable() == 3);
    CHECK(m.entries()[1].tick == 35301694);
    CHECK(m.entries()[2].tick == 108063452);
    CHECK(tick(m, 1) == 35);
    CHECK(tick(m, 999999) == 35301553);
    CHECK(tick(m, 1000003) == 35301694);
    CHECK(tick(m, 1000004) == 35301743);
    CHECK(tick(m, 2499999) == 108063403);
    CHECK(tick(m, 2500000) == 108063452);
    CHECK(tick(m, (Frames{1} << 47) + 12345) == 3119840533194140);
    CHECK(frame(m, 35301693) == 1000003);
    CHECK(frame(m, 120409130) == 3056919);
    CHECK(frame(m, kMaxMusical) == 406319041256524);
    // The last frame whose tick is in range, and the first that is not.
    CHECK(tick(m, 406319041256524) == kMaxMusical);
    Ticks t = 0;
    CHECK(!m.tick_at(406319041256525, t));
    CHECK(t == 0);
    return 0;
}

// Entry 0 extends backwards; halves round towards +inf by floor division.
int test_negative_rounding() {
    const MusicalMap m = map_of(44100, {TempoEntry{0, 97.3f, {{4, 4}}},
                                        TempoEntry{1000003, 133.7f, {{4, 4}}}});
    CHECK(tick(m, -1) == -35);
    CHECK(tick(m, -7) == -247);
    CHECK(tick(m, -17) == -600);
    CHECK(tick(m, -18) == -635);
    CHECK(tick(m, -(Frames{1} << 47) - 3) == -4968256887608321);
    CHECK(frame(m, -1) == 0);
    CHECK(frame(m, -17) == 0);
    CHECK(frame(m, -18) == -1);
    CHECK(frame(m, -22) == -1);
    CHECK(frame(m, kMinMusical) == -255149970886166);
    Ticks t = 0;
    CHECK(!m.tick_at(kMinMusical, t));   // its tick is below -2^53

    // 120 bpm at 48 kHz: 40 ticks a frame, so half a frame is 20 ticks.
    const MusicalMap even = map_of(48000, {TempoEntry{}});
    CHECK(frame(even, 20) == 1);
    CHECK(frame(even, 19) == 0);
    CHECK(frame(even, -20) == 0);   // -0.5 rounds up, to 0
    CHECK(frame(even, -21) == -1);
    CHECK(tick(even, -1) == -40);
    CHECK(tick(even, -24000) == -kTicksPerQuarter);
    return 0;
}

// Out-of-range results are refused, never wrapped: a high tempo at a low
// rate puts a frame well inside 2^53 at a tick past it.
int test_out_of_range_refused() {
    const MusicalMap fast = map_of(1, {TempoEntry{0, 1000.0f, {{4, 4}}}});
    Ticks t = 7;
    CHECK(fast.tick_at(Frames{1} << 29, t) && t == (Ticks{1} << 29) * 16'000'000);
    t = 7;
    CHECK(!fast.tick_at(Frames{1} << 30, t) && t == 7);
    CHECK(!fast.tick_at(kMaxMusical, t));
    CHECK(!fast.tick_at(kMaxMusical + 1, t));   // the argument itself
    CHECK(!fast.tick_at(std::numeric_limits<Frames>::min(), t));
    Frames f = 7;
    CHECK(!fast.frame_at(kMaxMusical + 1, f) && f == 7);

    const MusicalMap slow = map_of(384000, {TempoEntry{0, 1.0f, {{4, 4}}}});
    CHECK(!slow.frame_at(kMaxMusical, f) && f == 7);
    CHECK(slow.frame_at(kTicksPerQuarter, f) && f == 60 * 384000);

    // A later entry whose own tick is past 2^53: nothing from it on converts.
    const MusicalMap cut = map_of(1, {TempoEntry{0, 1000.0f, {{4, 4}}},
                                      TempoEntry{Frames{1} << 40, 120.0f, {{4, 4}}}});
    CHECK(cut.entries().size() == 2 && cut.reachable() == 1);
    CHECK(cut.tick_at(10, t) && t == 160'000'000);
    CHECK(!cut.tick_at(Frames{1} << 40, t));
    CHECK(!cut.tick_at((Frames{1} << 40) + 5, t));

    const MusicalMap empty;
    CHECK(!empty.tick_at(0, t) && !empty.frame_at(0, f));
    CHECK(empty.bars_beats_at(5) == BarsBeats{});
    return 0;
}

int test_bounds() {
    const std::vector<TempoEntry> ok{TempoEntry{}};
    MusicalMap m;
    std::string why;
    CHECK(m.build(384000, ok, why));
    CHECK(m.build(1, {TempoEntry{0, 1.0f, {{4, 1}}}, TempoEntry{9, 1000.0f, {{65535, 64}}}},
                  why));
    CHECK(refuses(ok, 0, "sample_rate: must be greater than 0"));
    CHECK(refuses(ok, 384001, "sample_rate: must be at most 384000"));
    CHECK(refuses({TempoEntry{0, 0.99f, {{4, 4}}}}, 48000, "tempo[0]: bpm must be from 1 to 1000"));
    CHECK(refuses({TempoEntry{}, TempoEntry{5, std::nextafter(1000.0f, 2000.0f), {{4, 4}}}}, 48000,
                  "tempo[1]: bpm must be from 1 to 1000"));
    CHECK(refuses({TempoEntry{0, 0.0f, {{4, 4}}}}, 48000, "bpm must be a positive number"));
    CHECK(refuses({TempoEntry{0, NAN, {{4, 4}}}}, 48000, "bpm must be a positive number"));
    CHECK(refuses({TempoEntry{0, 120.0f, {{4, 3}}}}, 48000, "power of two from 1 to 64"));
    CHECK(refuses({TempoEntry{0, 120.0f, {{4, 128}}}}, 48000, "power of two from 1 to 64"));
    CHECK(refuses({TempoEntry{0, 120.0f, {{0, 4}}}}, 48000, "two counts of at least 1"));
    CHECK(refuses({TempoEntry{0, 120.0f, {{65536, 4}}}}, 48000, "at most 65535 beats"));
    CHECK(refuses({}, 48000, "tempo: needs at least one entry"));
    CHECK(refuses({TempoEntry{3, 120.0f, {{4, 4}}}}, 48000, "the first entry must be at 0"));
    CHECK(refuses({TempoEntry{}, TempoEntry{9, 90.0f, {{4, 4}}}, TempoEntry{9, 80.0f, {{4, 4}}}},
                  48000, "tempo[2]: entries must be sorted"));

    for (std::uint32_t d = 1; d <= 64; d *= 2) CHECK(is_meter_denominator(d));
    for (std::uint32_t d : {0u, 3u, 5u, 6u, 12u, 48u, 128u}) CHECK(!is_meter_denominator(d));
    CHECK(nearest_meter_denominator(3) == 4);
    CHECK(nearest_meter_denominator(5) == 4);
    CHECK(nearest_meter_denominator(6) == 8);
    CHECK(nearest_meter_denominator(7) == 8);
    CHECK(nearest_meter_denominator(11) == 8);
    CHECK(nearest_meter_denominator(12) == 16);
    CHECK(nearest_meter_denominator(48) == 64);
    CHECK(nearest_meter_denominator(65535) == 64);
    CHECK(nearest_meter_denominator(32) == 32);
    CHECK(nearest_meter_denominator(0) == 0);
    CHECK(bpm_within_bounds(0.25f) == 1.0f);
    CHECK(bpm_within_bounds(4000.0f) == 1000.0f);
    CHECK(bpm_within_bounds(97.3f) == 97.3f);
    CHECK(bpm_within_bounds(-3.0f) == -3.0f);
    return 0;
}

// A random map inside the bounds, its entries at least `gap` frames apart.
std::vector<TempoEntry> random_map(std::mt19937_64& rng, std::size_t n, Frames gap,
                                   bool fine_ticks) {
    std::uniform_real_distribution<float> bpm(fine_ticks ? 13.0f : 1.0f, 1000.0f);
    std::uniform_int_distribution<int> den_log(0, 6);
    std::uniform_int_distribution<std::uint32_t> beats(1, 13);
    std::uniform_int_distribution<Frames> step(gap, gap * 50);
    std::vector<TempoEntry> map;
    Frames at = 0;
    for (std::size_t i = 0; i < n; ++i) {
        TempoEntry e;
        e.at = at;
        // Mixed on purpose: round tempi take the 64-bit path, the rest the
        // 128-bit one.
        e.bpm = i % 3 == 0 ? std::round(bpm(rng) * 2.0f) / 2.0f : bpm(rng);
        e.meter = {{beats(rng), 1u << den_log(rng)}};
        map.push_back(e);
        at += step(rng);
    }
    return map;
}

#if defined(__SIZEOF_INT128__)
// An independent reference: frexp for the float's exact value and GCC's
// __int128, against the portable 128-bit code under test.
struct Oracle {
    struct E {
        Frames at;
        Ticks tick;
        __int128 num, den;   // ticks per frame
    };
    std::vector<E> e;
    static __int128 round_div(__int128 x, __int128 y) {   // y > 0
        const __int128 n = 2 * x + y, d = 2 * y;
        __int128 q = n / d;
        if (n % d != 0 && n < 0) --q;
        return q;
    }
    Oracle(std::uint32_t rate, const std::vector<TempoEntry>& map) {
        for (const TempoEntry& t : map) {
            int exp = 0;
            const double mant = std::frexp(static_cast<double>(t.bpm), &exp);
            const auto m = static_cast<__int128>(std::ldexp(mant, 24));   // exact: 24 bits
            const int s = 24 - exp;   // bpm = m / 2^s, s > 0 for bpm < 2^24
            E x{t.at, 0, m * kTicksPerQuarter, (__int128{1} << s) * 60 * rate};
            if (!e.empty()) x.tick = e.back().tick + static_cast<Ticks>(round_div(
                                         (t.at - e.back().at) * e.back().num, e.back().den));
            e.push_back(x);
        }
    }
    bool tick(Frames f, Ticks& out) const {
        std::size_t k = 0;
        while (k + 1 < e.size() && e[k + 1].at <= f) ++k;
        const __int128 t = e[k].tick + round_div((f - e[k].at) * e[k].num, e[k].den);
        if (t < kMinMusical || t > kMaxMusical) return false;
        out = static_cast<Ticks>(t);
        return true;
    }
    bool frame(Ticks t, Frames& out) const {
        std::size_t k = 0;
        while (k + 1 < e.size() && e[k + 1].tick <= t) ++k;
        const __int128 f = e[k].at + round_div((t - e[k].tick) * e[k].den, e[k].num);
        if (f < kMinMusical || f > kMaxMusical) return false;
        out = static_cast<Frames>(f);
        return true;
    }
};

int test_against_reference() {
    std::mt19937_64 rng(14);
    const std::uint32_t rates[] = {1, 8000, 22050, 44100, 48000, 88200, 96000, 192000, 384000};
    std::uniform_int_distribution<std::int64_t> any(kMinMusical, kMaxMusical);
    std::uniform_int_distribution<std::int64_t> near(-5'000'000, 50'000'000);
    int compared = 0;
    for (int round = 0; round < 200; ++round) {
        const std::uint32_t rate = rates[round % 9];
        // Entries spread far, so the map's own ticks stay inside 2^53.
        const auto map = random_map(rng, 1 + round % 7, rate == 1 ? 10 : 100'000, false);
        const MusicalMap m = map_of(rate, map);
        const Oracle o(rate, map);
        if (m.reachable() != map.size()) continue;
        for (std::size_t k = 0; k < map.size(); ++k) CHECK(m.entries()[k].tick == o.e[k].tick);
        for (int i = 0; i < 2000; ++i) {
            const std::int64_t v = i % 2 == 0 ? any(rng) : near(rng);
            Ticks t = 0, ot = 0;
            const bool a = m.tick_at(v, t), b = o.tick(v, ot);
            if (a != b || (a && t != ot)) {
                std::fprintf(stderr, "  rate %u frame %lld: %d %lld, reference %d %lld\n", rate,
                             static_cast<long long>(v), a, static_cast<long long>(t), b,
                             static_cast<long long>(ot));
            }
            CHECK(a == b && (!a || t == ot));
            Frames f = 0, of = 0;
            const bool c = m.frame_at(v, f), d = o.frame(v, of);
            CHECK(c == d && (!c || f == of));
            ++compared;
        }
    }
    CHECK(compared > 100'000);
    return 0;
}
#endif

// midi.md §2.2's three properties, where ticks are finer than frames.
int test_properties() {
    std::mt19937_64 rng(2014);
    const std::uint32_t rates[] = {44100, 48000, 96000, 192000};
    for (int round = 0; round < 40; ++round) {
        const std::uint32_t rate = rates[round % 4];
        // Above 12 bpm a tick is shorter than a frame at 192 kHz.
        const auto map = random_map(rng, 2 + round % 9, 20'000, true);
        const MusicalMap m = map_of(rate, map);
        CHECK(m.reachable() == map.size());
        for (std::size_t k = 0; k < map.size(); ++k) {
            const Frames a = map[k].at;
            const Ticks tk = m.entries()[k].tick;
            CHECK(tick(m, a) == tk);
            CHECK(frame(m, tk) == a);
            if (k > 0) CHECK(tick(m, a - 1) < tk);
            for (Frames f = a - 300; f < a + 300; ++f) CHECK(frame(m, tick(m, f)) == f);
        }
        std::uniform_int_distribution<Frames> pick(-1'000'000, map.back().at + 10'000'000);
        std::vector<Frames> fs(4000);
        for (Frames& f : fs) f = pick(rng);
        std::sort(fs.begin(), fs.end());
        Ticks prev_t = kMinMusical;
        for (Frames f : fs) {
            const Ticks t = tick(m, f);
            CHECK(t >= prev_t);
            prev_t = t;
            CHECK(frame(m, t) == f);
        }
        std::vector<Ticks> ts(4000);
        std::uniform_int_distribution<Ticks> pick_t(-50'000'000, tick(m, map.back().at) * 2);
        for (Ticks& t : ts) t = pick_t(rng);
        std::sort(ts.begin(), ts.end());
        Frames prev_f = kMinMusical;
        for (Ticks t : ts) {
            const Frames f = frame(m, t);
            CHECK(f >= prev_f);
            prev_f = f;
        }
    }
    return 0;
}

int test_batch() {
    std::mt19937_64 rng(7);
    const auto map = random_map(rng, 40, 50'000, true);
    const MusicalMap m = map_of(48000, map);
    std::uniform_int_distribution<Frames> pick(-100'000, map.back().at + 1'000'000);
    std::vector<Frames> fs(20000);
    for (Frames& f : fs) f = pick(rng);
    std::vector<Ticks> ts(fs.size());
    std::vector<Frames> back(fs.size());
    for (bool sorted : {false, true}) {
        if (sorted) std::sort(fs.begin(), fs.end());
        CHECK(m.frames_to_ticks(fs.data(), fs.size(), ts.data()) == fs.size());
        for (std::size_t i = 0; i < fs.size(); ++i) CHECK(ts[i] == tick(m, fs[i]));
        CHECK(m.ticks_to_frames(ts.data(), ts.size(), back.data()) == ts.size());
        for (std::size_t i = 0; i < ts.size(); ++i) CHECK(back[i] == frame(m, ts[i]));
    }
    // It stops at the first conversion out of range and says where.
    const MusicalMap fast = map_of(1, {TempoEntry{0, 1000.0f, {{4, 4}}}});
    const Frames in[] = {1, 2, Frames{1} << 30, 3};
    Ticks out[4] = {};
    CHECK(fast.frames_to_ticks(in, 4, out) == 2);
    CHECK(out[1] == 32'000'000);
    CHECK(MusicalMap{}.frames_to_ticks(in, 4, out) == 0);
    return 0;
}

// --- bars|beats ---------------------------------------------------------------

constexpr std::uint32_t kRate = 48000;
constexpr Frames kBeat120 = 24000;
constexpr Frames kBar120 = 96000;
constexpr Frames kBeat96 = 30000;
constexpr Frames kBar96 = 120000;
constexpr Frames kAt96 = 16 * kBar120;
constexpr Frames kAt34 = kAt96 + 8 * kBar96;

BarsBeats read(const MusicalMap& m, Frames f) { return m.bars_beats_at(tick(m, f)); }

Frames frame_of(const MusicalMap& m, BarsBeats b) {
    Ticks t = 0;
    if (!m.tick_of(b, t)) return -1;
    return frame(m, t);
}

int test_bars_beats() {
    const MusicalMap s = map_of(kRate, {TempoEntry{0, 120.0f, {{4, 4}}},
                                        TempoEntry{kAt96, 96.0f, {{4, 4}}},
                                        TempoEntry{kAt34, 96.0f, {{3, 4}}}});
    CHECK(read(s, 0) == (BarsBeats{1, 1, 0}));
    CHECK(read(s, kBeat120 / 2) == (BarsBeats{1, 1, 480}));
    CHECK(read(s, kAt96 - kBeat120) == (BarsBeats{16, 4, 0}));
    CHECK(read(s, kAt96) == (BarsBeats{17, 1, 0}));
    CHECK(read(s, kAt96 + kBeat96 / 2) == (BarsBeats{17, 1, 480}));
    CHECK(read(s, kAt34) == (BarsBeats{25, 1, 0}));
    CHECK(read(s, kAt34 + 3 * kBeat96) == (BarsBeats{26, 1, 0}));
    CHECK(read(s, -1) == (BarsBeats{0, 4, 959}));
    CHECK(frame_of(s, {17, 2, 0}) == kAt96 + kBeat96);
    CHECK(frame_of(s, {26, 1, 0}) == kAt34 + 3 * kBeat96);
    CHECK(frame_of(s, {1, 1, 480}) == kBeat120 / 2);
    CHECK(frame_of(s, {0, 3, 0}) == 0);   // before 1|1|000

    // A tempo change inside a bar carries the count on.
    const MusicalMap mid = map_of(kRate, {TempoEntry{0, 120.0f, {{4, 4}}},
                                          TempoEntry{6 * kBeat120, 60.0f, {{4, 4}}}});
    CHECK(read(mid, 6 * kBeat120) == (BarsBeats{2, 3, 0}));
    CHECK(read(mid, 6 * kBeat120 + 2 * 48000) == (BarsBeats{3, 1, 0}));

    // A meter change inside a bar starts one; the bar it cut is counted, and
    // its beats past the change name the change.
    const MusicalMap cut = map_of(kRate, {TempoEntry{0, 120.0f, {{4, 4}}},
                                          TempoEntry{2 * kBeat120, 120.0f, {{2, 4}}}});
    CHECK(read(cut, 2 * kBeat120) == (BarsBeats{2, 1, 0}));
    CHECK(read(cut, 3 * kBeat120) == (BarsBeats{2, 2, 0}));
    CHECK(frame_of(cut, {1, 3, 0}) == 2 * kBeat120);
    CHECK(frame_of(cut, {1, 4, 480}) == 2 * kBeat120);
    CHECK(frame_of(cut, {2, 2, 0}) == 3 * kBeat120);

    // Within half a display tick of a bar line, a meter change is on it:
    // one frame either side (40 ticks) counts no extra bar; 100 frames past
    // (4,000 ticks, more than 500) leaves a sliver that is counted.
    for (Frames off : {Frames{-1}, Frames{1}}) {
        const MusicalMap on = map_of(kRate, {TempoEntry{0, 120.0f, {{4, 4}}},
                                             TempoEntry{kBar120 + off, 120.0f, {{3, 4}}}});
        CHECK(read(on, kBar120 + off) == (BarsBeats{2, 1, 0}));
    }
    const MusicalMap sliver = map_of(kRate, {TempoEntry{0, 120.0f, {{4, 4}}},
                                             TempoEntry{kBar120 + 100, 120.0f, {{3, 4}}}});
    CHECK(read(sliver, kBar120) == (BarsBeats{2, 1, 0}));
    CHECK(read(sliver, kBar120 + 100) == (BarsBeats{3, 1, 0}));

    // A bar of n/d is n * 3,840,000 / d ticks; 960 display ticks a beat even
    // when a beat (60,000 ticks at /64) is not a multiple of 960.
    const MusicalMap tiny = map_of(kRate, {TempoEntry{0, 120.0f, {{7, 64}}}});
    CHECK(tiny.bars_beats_at(7 * 60000) == (BarsBeats{2, 1, 0}));
    CHECK(tiny.bars_beats_at(60000 + 30000) == (BarsBeats{1, 2, 480}));
    CHECK(tiny.bars_beats_at(62) == (BarsBeats{1, 1, 0}));
    CHECK(tiny.bars_beats_at(63) == (BarsBeats{1, 1, 1}));
    Ticks t = 0;
    CHECK(tiny.tick_of({1, 1, 1}, t) && t == 63);   // 62.5 rounds up
    CHECK(tiny.tick_of({1, 2, 480}, t) && t == 90000);

    // Every display tick of a stretch round-trips through its reading.
    const MusicalMap eight = map_of(kRate, {TempoEntry{0, 97.3f, {{6, 8}}},
                                            TempoEntry{777'777, 141.0f, {{5, 16}}}});
    for (Ticks at = 0; at < 200'000'000; at += 499'500) {
        const BarsBeats b = eight.bars_beats_at(at);
        Ticks back = 0;
        CHECK(eight.tick_of(b, back));
        CHECK(eight.bars_beats_at(back) == b);
        CHECK(back <= at && at - back < kTicksPerWhole / 8 / 960 + 1);
    }
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    int run = 0;
    const auto t = [&](int (*f)()) {
        ++run;
        failed += f();
    };
    t(test_exact_values);
    t(test_negative_rounding);
    t(test_out_of_range_refused);
    t(test_bounds);
#if defined(__SIZEOF_INT128__)
    t(test_against_reference);
#endif
    t(test_properties);
    t(test_batch);
    t(test_bars_beats);
    if (failed != 0) {
        std::fprintf(stderr, "model_musical_map: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_musical_map: %d passed\n", run);
    return 0;
}
