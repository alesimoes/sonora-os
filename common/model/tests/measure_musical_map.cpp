// SPDX-License-Identifier: GPL-3.0-or-later
//
// What MusicalMap costs the control thread (midi.md §2.2, "Cost"; ADR-0014
// §2): a million conversions, 500,000 events' worth, against the 10 ms
// budget. Run by hand; the figures go in the M-1 PR. Not a ctest: a
// shared CI runner's timings are not the appliance's.
//
//   measure_musical_map [conversions]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "sonora/model/musical_map.h"

using namespace sonora::model;

namespace {

using Clock = std::chrono::steady_clock;

// Best of five, in nanoseconds per conversion.
template <typename F>
double best_ns(std::size_t n, F&& run) {
    double best = 1e300;
    for (int i = 0; i < 5; ++i) {
        const auto t0 = Clock::now();
        run();
        const auto t1 = Clock::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    return best / static_cast<double>(n);
}

struct Case {
    const char* name;
    std::uint32_t rate;
    std::vector<TempoEntry> tempo;
    Frames span;   // events fall in [0, span) frames
};

// The share of conversions the 64-bit fast path takes, frames -> ticks and
// ticks -> frames, so each figure says which arithmetic it measured.
void fast_share(const MusicalMap& m, const std::vector<Frames>& fs, const std::vector<Ticks>& ts,
                double& f2t, double& t2f) {
    std::size_t a = 0, b = 0;
    for (std::size_t i = 0; i < fs.size(); ++i) {
        const MusicalMap::Entry& e = m.entries()[m.entry_at_frame(fs[i])];
        a += static_cast<std::uint64_t>(std::llabs(fs[i] - e.at)) <= e.fast_frames;
        const MusicalMap::Entry& g = m.entries()[m.entry_at_tick(ts[i])];
        b += static_cast<std::uint64_t>(std::llabs(ts[i] - g.tick)) <= g.fast_ticks;
    }
    f2t = 100.0 * static_cast<double>(a) / static_cast<double>(fs.size());
    t2f = 100.0 * static_cast<double>(b) / static_cast<double>(fs.size());
}

std::vector<TempoEntry> song(std::uint32_t rate, bool round, std::size_t changes) {
    std::mt19937 rng(6);
    std::uniform_real_distribution<float> bpm(60.0f, 180.0f);
    std::vector<TempoEntry> map;
    for (std::size_t i = 0; i < changes; ++i) {
        const float b = round ? std::round(bpm(rng)) : bpm(rng);
        map.push_back(TempoEntry{static_cast<Frames>(i) * 16 * rate, b, {{4, 4}}});
    }
    return map;
}

volatile std::int64_t g_sink = 0;

}  // namespace

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
    // Five minutes of a song, except the last: one entry and events spread
    // over 2^45 frames, far enough from it that the 128-bit path is taken.
    const std::vector<Case> cases = {
        {"120 bpm, 48 kHz, one entry, 5 min", 48000, {TempoEntry{}}, 48000 * 300},
        {"integer bpm, 48 kHz, 18 entries, 5 min", 48000, song(48000, true, 18), 48000 * 300},
        {"non-integer bpm, 44.1 kHz, 18 entries, 5 min", 44100, song(44100, false, 18),
         44100 * 300},
        {"non-integer bpm, 192 kHz, 18 entries, 5 min", 192000, song(192000, false, 18),
         192000 * 300},
        {"97.3 bpm, 44.1 kHz, one entry, 2^45 frames", 44100, {TempoEntry{0, 97.3f, {{4, 4}}}},
         Frames{1} << 45},
    };
#if defined(__clang__)
    std::printf("compiler: clang %s\n", __clang_version__);
#elif defined(__GNUC__)
    std::printf("compiler: GCC %s\n", __VERSION__);
#elif defined(_MSC_VER)
    std::printf("compiler: MSVC %d\n", _MSC_VER);
#endif
    std::printf("%zu conversions per run, best of 5; the control thread's budget is 10 ms\n\n", n);
    for (const Case& c : cases) {
        MusicalMap m;
        std::string why;
        if (!m.build(c.rate, c.tempo, why)) {
            std::fprintf(stderr, "%s: %s\n", c.name, why.c_str());
            return 1;
        }
        // Sorted, as a plan builder walks them.
        std::mt19937_64 rng(1);
        std::uniform_int_distribution<Frames> pick(0, c.span - 1);
        std::vector<Frames> frames(n);
        for (Frames& f : frames) f = pick(rng);
        std::sort(frames.begin(), frames.end());
        std::vector<Ticks> ticks(n);
        std::vector<Frames> back(n);
        if (m.frames_to_ticks(frames.data(), n, ticks.data()) != n) return 1;
        double fast_f2t = 0, fast_t2f = 0;
        fast_share(m, frames, ticks, fast_f2t, fast_t2f);

        const double f2t = best_ns(n, [&] {
            g_sink = static_cast<std::int64_t>(m.frames_to_ticks(frames.data(), n, ticks.data()));
        });
        const double t2f = best_ns(n, [&] {
            g_sink = static_cast<std::int64_t>(m.ticks_to_frames(ticks.data(), n, back.data()));
        });
        const double one = best_ns(n, [&] {
            std::int64_t sum = 0;
            for (std::size_t i = 0; i < n; ++i) {
                Frames f = 0;
                m.frame_at(ticks[i], f);
                sum += f;
            }
            g_sink = sum;
        });
        if (back != frames) {
            std::fprintf(stderr, "%s: a frame did not come back\n", c.name);
            return 1;
        }
        // n ns each is n ms a million.
        std::printf("%s\n  64-bit path: %.0f%% of frames -> ticks, %.0f%% of ticks -> frames\n",
                    c.name, fast_f2t, fast_t2f);
        std::printf("  frames -> ticks, batch  %6.1f ns, %6.2f ms a million\n", f2t, f2t);
        std::printf("  ticks -> frames, batch  %6.1f ns, %6.2f ms a million\n", t2f, t2f);
        std::printf("  ticks -> frames, single %6.1f ns, %6.2f ms a million\n", one, one);
    }
    return 0;
}
