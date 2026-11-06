// SPDX-License-Identifier: GPL-3.0-or-later
//
// The track and master meter (meter.h) and the O(log bins) loudness
// statistics under it (loudness.h):
// - the Fenwick-tree integrated loudness and LRA against the bin scan they
//   replace, on the same block values (bit-for-bit gate decisions), and
//   against BS.1770-5 / Tech 3342 computed exactly from the unbinned blocks;
// - the published readings against a plain LoudnessMeter, and EBU Tech
//   3341 case 1 (1 kHz at -23 dBFS reads -23.0 LUFS);
// - peak: instant rise, 20 dB in 1.7 s fall (IEC 60268-10 Type I), take
//   clears, and a reader on another thread misses no peak;
// - true-peak: an fs/4 sine sampled 45 degrees off its crest;
// - non-finite input counted and metered as silence; denormals; no
//   allocation in process() or a reader; a long call metered as its pieces;
//   the operator's reset;
// - CPU per block at q128, 48 kHz, stereo (ADR-0011 §3.5, Amendment 1 item 4).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <thread>
#include <vector>

#include "check.h"
#include "sonora/dsp/biquad.h"
#include "sonora/dsp/denormals.h"
#include "sonora/dsp/loudness.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/meter.h"
#include "sonora/dsp/true_peak.h"

namespace {
std::atomic<long> g_allocations{0};
// Keeps a timed loop's result alive.
volatile double g_sink = 0.0;
}  // namespace

void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size > 0 ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace sonora::dsp;

using Chans = std::vector<std::vector<float>>;

// Deterministic, so a failure reproduces.
struct Rng {
    std::uint64_t s = 0x9E3779B97F4A7C15ull;
    double uniform() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0);
    }
};

// Programme with a wide loudness spread: 1 kHz and 3 kHz tones whose level
// jumps every 0.37 s to a level between -80 and -3 dBFS, so the absolute
// and both relative gates all remove blocks.
Chans programme(double fs, double seconds, std::uint32_t channels = 2) {
    const auto n = static_cast<std::size_t>(std::llround(seconds * fs));
    const auto seg = static_cast<std::size_t>(std::llround(0.37 * fs));
    Chans x(channels, std::vector<float>(n));
    Rng rng;
    double g = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        if (i % seg == 0) g = db_to_gain(-80.0 + 77.0 * rng.uniform());
        const double t = static_cast<double>(i) / fs;
        for (std::uint32_t c = 0; c < channels; ++c) {
            const double f = c == 0 ? 1000.0 : 3000.0;
            x[c][i] = static_cast<float>(g * std::sin(kTwoPi * f * t));
        }
    }
    return x;
}

Chans sine(double fs, double seconds, double freq, double dbfs, std::uint32_t channels = 2,
           double phase = 0.0) {
    const auto n = static_cast<std::size_t>(std::llround(seconds * fs));
    Chans x(channels, std::vector<float>(n));
    const double a = db_to_gain(dbfs);
    for (std::size_t i = 0; i < n; ++i) {
        const auto v = static_cast<float>(a * std::sin(kTwoPi * freq * i / fs + phase));
        for (auto& c : x) c[i] = v;
    }
    return x;
}

template <typename Meter>
void feed(Meter& m, const Chans& x, std::size_t block = 128) {
    const std::size_t n = x[0].size();
    std::vector<const float*> ptr(x.size());
    for (std::size_t pos = 0; pos < n; pos += block) {
        const auto len = static_cast<std::uint32_t>(std::min(block, n - pos));
        for (std::size_t c = 0; c < x.size(); ++c) ptr[c] = &x[c][pos];
        m.process(ptr.data(), len);
    }
}

double to_lufs(double ms) {
    if (!(ms > 0.0)) return LoudnessMeter::kFloorLufs;
    const double l = -0.691 + 10.0 * std::log10(ms);
    return l < LoudnessMeter::kFloorLufs ? LoudnessMeter::kFloorLufs : l;
}

// The gating blocks of a programme, computed outside the meter with the
// meter's own arithmetic in the meter's order, so the values are the ones
// the meter bins.
struct Blocks {
    std::vector<double> m_lufs, m_ms;  // 400 ms blocks every 100 ms
    std::vector<double> s_lufs, s_ms;  // 3 s blocks every 100 ms
};

Blocks blocks_of(double fs, const Chans& x) {
    BiquadCoeffs k1, k2;
    k_weighting(fs, k1, k2);
    std::vector<BiquadState> s1(x.size()), s2(x.size());
    const auto sub_len = static_cast<std::uint32_t>(std::lround(fs / 100.0));
    std::vector<double> subs;
    double sub_sum = 0.0;
    std::uint32_t fill = 0;
    Blocks b;
    for (std::size_t i = 0; i < x[0].size(); ++i) {
        double sum = 0.0;
        for (std::size_t c = 0; c < x.size(); ++c) {
            const double y = s2[c].tick(k2, s1[c].tick(k1, static_cast<double>(x[c][i])));
            sum += 1.0 * y * y;
        }
        sub_sum += sum;
        if (++fill < sub_len) continue;
        subs.push_back(sub_sum / sub_len);
        sub_sum = 0.0;
        fill = 0;
        const std::size_t steps = subs.size();
        if (steps % 10 != 0) continue;
        // Newest first, as LoudnessMeter::finish_sub_block sums.
        double m = 0.0, s = 0.0;
        for (std::size_t k = 0; k < 300; ++k) {
            const double v = k < steps ? subs[steps - 1 - k] : 0.0;
            if (k < 40) m += v;
            s += v;
        }
        if (steps >= 40) {
            b.m_ms.push_back(m / 40);
            b.m_lufs.push_back(to_lufs(m / 40));
        }
        if (steps >= 300) {
            b.s_ms.push_back(s / 300);
            b.s_lufs.push_back(to_lufs(s / 300));
        }
    }
    return b;
}

// The bin scan the Fenwick trees replace (loudness.cpp before this change),
// on a histogram built here from the same block values.
struct ScanHist {
    static constexpr double kMin = -70.0, kBin = 0.01;
    std::vector<std::uint64_t> count = std::vector<std::uint64_t>(10000, 0);
    std::vector<double> energy = std::vector<double>(10000, 0.0);
    void add(double lufs, double e) {
        if (lufs < kMin) return;
        double idx = std::floor((lufs - kMin) / kBin);
        if (idx > 9999.0) idx = 9999.0;
        const auto i = static_cast<std::size_t>(idx);
        ++count[i];
        energy[i] += e;
    }
};

double scan_integrated(const ScanHist& h) {
    double e = 0.0;
    std::uint64_t n = 0;
    for (std::uint32_t i = 0; i < 10000; ++i) {
        e += h.energy[i];
        n += h.count[i];
    }
    if (n == 0) return LoudnessMeter::kFloorLufs;
    const double rel = to_lufs(e / n) - 10.0;
    double e2 = 0.0;
    std::uint64_t n2 = 0;
    for (std::uint32_t i = 0; i < 10000; ++i) {
        if (ScanHist::kMin + i * ScanHist::kBin >= rel) {
            e2 += h.energy[i];
            n2 += h.count[i];
        }
    }
    return n2 == 0 ? LoudnessMeter::kFloorLufs : to_lufs(e2 / n2);
}

double scan_lra(const ScanHist& h) {
    double e = 0.0;
    std::uint64_t n = 0;
    for (std::uint32_t i = 0; i < 10000; ++i) {
        e += h.energy[i];
        n += h.count[i];
    }
    if (n == 0) return 0.0;
    const double rel = to_lufs(e / n) - 20.0;
    std::uint32_t first = 0;
    std::uint64_t m = 0;
    for (std::uint32_t i = 10000; i-- > 0;) {
        if (ScanHist::kMin + i * ScanHist::kBin < rel) {
            first = i + 1;
            break;
        }
        m += h.count[i];
    }
    if (m == 0) return 0.0;
    auto at = [&](std::uint64_t r) {
        std::uint64_t seen = 0;
        for (std::uint32_t i = first; i < 10000; ++i) {
            seen += h.count[i];
            if (seen >= r) return ScanHist::kMin + (i + 0.5) * ScanHist::kBin;
        }
        return 30.0;
    };
    const auto lo = static_cast<std::uint64_t>(std::llround((m - 1) * 0.10 + 1.0));
    const auto hi = static_cast<std::uint64_t>(std::llround((m - 1) * 0.95 + 1.0));
    return at(hi) - at(lo);
}

// BS.1770-5 §2.8 and Tech 3342 §2, unbinned.
double exact_integrated(const Blocks& b) {
    double e = 0.0;
    std::size_t n = 0;
    for (std::size_t j = 0; j < b.m_lufs.size(); ++j) {
        if (b.m_lufs[j] > -70.0) {
            e += b.m_ms[j];
            ++n;
        }
    }
    if (n == 0) return LoudnessMeter::kFloorLufs;
    const double rel = to_lufs(e / n) - 10.0;
    double e2 = 0.0;
    std::size_t n2 = 0;
    for (std::size_t j = 0; j < b.m_lufs.size(); ++j) {
        if (b.m_lufs[j] > -70.0 && b.m_lufs[j] > rel) {
            e2 += b.m_ms[j];
            ++n2;
        }
    }
    return n2 == 0 ? LoudnessMeter::kFloorLufs : to_lufs(e2 / n2);
}

double exact_lra(const Blocks& b) {
    double e = 0.0;
    std::size_t n = 0;
    for (std::size_t j = 0; j < b.s_lufs.size(); ++j) {
        if (b.s_lufs[j] > -70.0) {
            e += b.s_ms[j];
            ++n;
        }
    }
    if (n == 0) return 0.0;
    const double rel = to_lufs(e / n) - 20.0;
    std::vector<double> v;
    for (double l : b.s_lufs) {
        if (l > -70.0 && l > rel) v.push_back(l);
    }
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const auto at = [&](double p) {
        return v[static_cast<std::size_t>(std::llround((v.size() - 1) * p / 100.0))];
    };
    return at(95.0) - at(10.0);
}

int test_fenwick_against_scan_and_exact(double seconds) {
    const double fs = 48000.0;
    const Chans x = programme(fs, seconds);
    // Read at several points of the programme, as a live meter would.
    const std::size_t n = x[0].size();
    const std::size_t step = n / 5;
    int bad = 0;
    for (std::size_t end = step; end <= n; end += step) {
        Chans part(2);
        for (std::size_t c = 0; c < 2; ++c) part[c].assign(x[c].begin(), x[c].begin() + end);
        LoudnessMeter mp;
        CHECK(mp.prepare(fs, 2, nullptr, false));
        feed(mp, part);
        const Blocks b = blocks_of(fs, part);
        ScanHist gate, lra;
        for (std::size_t j = 0; j < b.m_lufs.size(); ++j) gate.add(b.m_lufs[j], b.m_ms[j]);
        for (std::size_t j = 0; j < b.s_lufs.size(); ++j) lra.add(b.s_lufs[j], b.s_ms[j]);
        const double i_f = mp.integrated_lufs(), i_s = scan_integrated(gate),
                     i_x = exact_integrated(b);
        const double r_f = mp.loudness_range_lu(), r_s = scan_lra(lra), r_x = exact_lra(b);
        std::printf("  %6.1f s: integrated fenwick %.6f scan %.6f exact %.6f; "
                    "LRA fenwick %.4f scan %.4f exact %.4f\n",
                    end / fs, i_f, i_s, i_x, r_f, r_s, r_x);
        // Same bins, same gate decisions: only the order of the energy sums
        // differs.
        bad += !(std::fabs(i_f - i_s) <= 1e-9);
        bad += !(std::fabs(r_f - r_s) <= 1e-9);
        // The bins quantise each block to 0.01 LU (loudness.h).
        bad += !(std::fabs(i_f - i_x) <= 0.02);
        bad += !(std::fabs(r_f - r_x) <= 0.02);
    }
    CHECK(bad == 0);
    return 0;
}

int test_readings_match_loudness_meter() {
    const double fs = 48000.0;
    const Chans x = programme(fs, 20.0);
    LoudnessMeter ref;
    CHECK(ref.prepare(fs, 2));
    TrackMeter tm;
    CHECK(tm.prepare(fs, 128, 2));
    feed(ref, x);
    feed(tm, x);
    CHECK(tm.loudness_steps() == ref.steps());
    CHECK(tm.momentary_lufs() == static_cast<float>(ref.momentary_lufs()));
    CHECK(tm.short_term_lufs() == static_cast<float>(ref.short_term_lufs()));
    CHECK(tm.max_momentary_lufs() == static_cast<float>(ref.max_momentary_lufs()));
    CHECK(tm.max_short_term_lufs() == static_cast<float>(ref.max_short_term_lufs()));
    // 20 s is a whole number of 100 ms gate steps: nothing pending.
    CHECK(tm.integrated_lufs() == static_cast<float>(ref.integrated_lufs()));
    CHECK(tm.loudness_range_lu() == static_cast<float>(ref.loudness_range_lu()));
    CHECK(std::fabs(tm.true_peak_max_dbtp() - ref.true_peak_dbtp()) <= 1e-4);
    std::printf("  published = LoudnessMeter: M %.3f S %.3f I %.3f LRA %.3f TP %.3f\n",
                tm.momentary_lufs(), tm.short_term_lufs(), tm.integrated_lufs(),
                tm.loudness_range_lu(), tm.true_peak_max_dbtp());

    // EBU Tech 3341 case 1: stereo 1 kHz at -23 dBFS for 20 s.
    TrackMeter t1;
    CHECK(t1.prepare(fs, 128, 2));
    feed(t1, sine(fs, 20.0, 1000.0, -23.0));
    std::printf("  Tech 3341 case 1: M %.3f S %.3f I %.3f (want -23.0 +-0.1)\n",
                t1.momentary_lufs(), t1.short_term_lufs(), t1.integrated_lufs());
    CHECK(std::fabs(t1.momentary_lufs() + 23.0f) <= 0.1f);
    CHECK(std::fabs(t1.short_term_lufs() + 23.0f) <= 0.1f);
    CHECK(std::fabs(t1.integrated_lufs() + 23.0f) <= 0.1f);
    return 0;
}

int test_peak_ballistics() {
    const double fs = 48000.0;
    TrackMeter m;
    TrackMeter::Options o;
    o.loudness = false;
    o.true_peak = false;
    CHECK(m.prepare(fs, 512, 1, o));
    // An impulse as the last sample of a block: the reading is the sample,
    // no attack time.
    std::vector<float> blk(128, 0.0f);
    blk[127] = 0.5f;
    const float* p = blk.data();
    m.process(&p, 128);
    CHECK(m.display_peak(0) == 0.5f);
    CHECK(m.take_peak(0) == 0.5f);
    CHECK(m.take_peak(0) == 0.0f);  // the take cleared it
    // Then 1.7 s of silence: 20 dB down.
    std::vector<float> zero(1700 * 48, 0.0f);
    const float* z = zero.data();
    m.process(&z, static_cast<std::uint32_t>(zero.size()));
    const double fell = gain_to_db(0.5) - gain_to_db(m.display_peak(0));
    std::printf("  fall over 1.7 s: %.4f dB (want 20 +-0.01)\n", fell);
    CHECK(std::fabs(fell - 20.0) <= 0.01);
    CHECK(m.take_peak(0) == 0.0f);
    // Loudness off: the loudness readings stay at their floor.
    CHECK(m.momentary_lufs() == TrackMeter::kFloorDb);
    CHECK(m.integrated_lufs() == TrackMeter::kFloorDb);
    return 0;
}

int test_true_peak() {
    // fs/4 at 45 degrees: every sample is at 0.7071 of the crest, which
    // falls half-way between two samples, on the 4x grid.
    const double fs = 48000.0;
    TrackMeter m;
    CHECK(m.prepare(fs, 128, 1));
    const Chans x = sine(fs, 0.5, fs / 4.0, -6.0, 1, kTwoPi / 8.0);
    // The abrupt start rings the interpolator (a real inter-sample overshoot
    // of the signal as given, 0.1 dB here); the steady state is read after.
    const Chans onset{std::vector<float>(x[0].begin(), x[0].begin() + 4800)};
    const Chans steady{std::vector<float>(x[0].begin() + 4800, x[0].end())};
    feed(m, onset);
    m.take_peak(0);
    m.take_true_peak(0);
    feed(m, steady);
    const double sp = gain_to_db(m.take_peak(0));
    const double tp = gain_to_db(m.take_true_peak(0));
    std::printf("  fs/4 at -6 dBFS, 45 deg: sample peak %.3f dBFS, true peak %.3f dBTP\n", sp, tp);
    CHECK(std::fabs(sp - (-6.0 - 3.0103)) <= 0.01);
    // Tech 3341's true-peak tolerance, +0.2 / -0.4 dB.
    CHECK(tp + 6.0 <= 0.2 && tp + 6.0 >= -0.4);
    CHECK(m.true_peak_max_dbtp() >= static_cast<float>(tp));
    CHECK(m.take_true_peak(0) == 0.0f);
    return 0;
}

int test_true_peak_block_equals_per_sample() {
    // The meter's block interpolator against the per-sample one the limiter
    // and the Tech 3341 tests use: the same filter in single precision.
    TruePeakDetector a, b;
    a.prepare(1);
    b.prepare(1);
    b.prepare_blocks(128);
    Rng rng;
    std::vector<float> x(48000);
    for (std::size_t i = 0; i < x.size(); ++i) {
        // Full-band noise, then a 19 kHz tone, so every phase is exercised.
        x[i] = i < 24000 ? static_cast<float>(2.0 * rng.uniform() - 1.0)
                         : static_cast<float>(std::sin(kTwoPi * 19000.0 * i / 48000.0));
    }
    double worst = 0.0;
    const std::uint32_t sizes[] = {128, 1, 77, 128, 64, 3};
    std::size_t pos = 0;
    for (std::size_t j = 0; pos < x.size(); ++j) {
        const auto n = static_cast<std::uint32_t>(
            std::min<std::size_t>(sizes[j % 6], x.size() - pos));
        double want = 0.0;
        for (std::uint32_t k = 0; k < n; ++k) want = std::max(want, a.process(0, x[pos + k]));
        const double got = b.process_block(0, &x[pos], n);
        worst = std::max(worst, std::fabs(got - want));
        pos += n;
    }
    std::printf("  true-peak block vs per-sample: largest difference %.3g\n", worst);
    // Single precision: a few ulp of float at full scale (true_peak.h).
    CHECK(worst <= 2e-6);
    return 0;
}

int test_concurrent_reader_misses_no_peak() {
    const double fs = 48000.0;
    TrackMeter m;
    TrackMeter::Options o;
    o.loudness = false;
    CHECK(m.prepare(fs, 128, 1, o));
    Rng rng;
    std::vector<float> x(48000 * 4);
    float want = 0.0f;
    for (auto& v : x) {
        v = static_cast<float>(rng.uniform() * 0.5);
        want = std::max(want, v);
    }
    std::atomic<bool> done{false};
    float seen = 0.0f;
    std::thread reader([&] {
        while (!done.load(std::memory_order_acquire)) seen = std::max(seen, m.take_peak(0));
    });
    for (std::size_t pos = 0; pos < x.size(); pos += 128) {
        const float* p = &x[pos];
        m.process(&p, 128);
    }
    done.store(true, std::memory_order_release);
    reader.join();
    seen = std::max(seen, m.take_peak(0));
    std::printf("  concurrent takes: max %.6f, input max %.6f\n", seen, want);
    CHECK(seen == want);
    return 0;
}

int test_nonfinite_input() {
    const double fs = 48000.0;
    TrackMeter m;
    CHECK(m.prepare(fs, 128, 2));
    Chans x = sine(fs, 6.0, 1000.0, -23.0);
    const float bad[] = {std::nanf(""), INFINITY, -INFINITY};
    std::uint64_t injected = 0;
    for (std::size_t i = 4800; i < x[0].size(); i += 9973) {
        x[i % 2][i] = bad[injected % 3];
        ++injected;
    }
    feed(m, x);
    const float r[] = {m.momentary_lufs(), m.short_term_lufs(), m.integrated_lufs(),
                       m.loudness_range_lu(), m.true_peak_max_dbtp(), m.display_peak(0),
                       m.take_peak(0), m.take_true_peak(1)};
    for (float v : r) CHECK(std::isfinite(v));
    std::printf("  %llu non-finite samples injected, %llu counted; M %.3f after them\n",
                static_cast<unsigned long long>(injected),
                static_cast<unsigned long long>(m.nonfinite_samples()), m.momentary_lufs());
    CHECK(m.nonfinite_samples() == injected);
    // Metered as single silent samples, the tone still reads its level.
    CHECK(std::fabs(m.momentary_lufs() + 23.0f) <= 0.1f);

    // Finite but huge (#315 review): one block doubling from 1e30 to
    // 2.68e38, near FLT_MAX. The single-precision interpolator overflowed
    // and held true-peak at +inf until reset. Then a tone: no filter state
    // may stay poisoned.
    TrackMeter h;
    CHECK(h.prepare(fs, 128, 2));
    std::vector<float> huge(128);
    for (std::size_t i = 0; i < huge.size(); ++i) {
        const auto k = static_cast<int>(std::min<std::size_t>(i, 28));
        huge[i] = static_cast<float>(1e30 * std::ldexp(1.0, k));
    }
    const float* hp[2] = {huge.data(), huge.data()};
    h.process(hp, 128);
    const float hr[] = {h.take_true_peak(0), h.true_peak_max_dbtp(), h.take_peak(0),
                        h.display_peak(1), h.momentary_lufs(), h.short_term_lufs()};
    for (float v : hr) CHECK(std::isfinite(v));
    CHECK(h.nonfinite_samples() == 0);
    // The interpolator's tail of the huge block reaches 16 samples into the
    // tone (true_peak.h's delay); channel 1's take starts after it.
    const Chans tone = sine(fs, 4.0, 1000.0, -23.0);
    feed(h, Chans{std::vector<float>(tone[0].begin(), tone[0].begin() + 4800),
                  std::vector<float>(tone[1].begin(), tone[1].begin() + 4800)});
    h.take_true_peak(1);
    feed(h, Chans{std::vector<float>(tone[0].begin() + 4800, tone[0].end()),
                  std::vector<float>(tone[1].begin() + 4800, tone[1].end())});
    const float after[] = {h.take_true_peak(0), h.true_peak_max_dbtp(), h.momentary_lufs(),
                           h.short_term_lufs(), h.integrated_lufs(), h.loudness_range_lu(),
                           h.display_peak(0)};
    for (float v : after) CHECK(std::isfinite(v));
    const double tp_tone = gain_to_db(h.take_true_peak(1));
    std::printf("  huge finite block: TP max %.1f dBTP; then a -23 dBFS tone reads M %.3f, "
                "TP %.3f dBTP\n",
                h.true_peak_max_dbtp(), h.momentary_lufs(), tp_tone);
    CHECK(std::fabs(h.momentary_lufs() + 23.0f) <= 0.1f);
    CHECK(std::fabs(tp_tone + 23.0) <= 0.1);
    return 0;
}

int test_long_call_equals_pieces() {
    const double fs = 44100.0;
    const Chans x = programme(fs, 7.0);
    TrackMeter a, b;
    CHECK(a.prepare(fs, 128, 2));
    CHECK(b.prepare(fs, 4096, 2));
    feed(a, x, 1000);  // every call longer than max_frames
    feed(b, x, 128);
    CHECK(a.loudness_steps() == b.loudness_steps());
    CHECK(a.momentary_lufs() == b.momentary_lufs());
    CHECK(a.integrated_lufs() == b.integrated_lufs());
    CHECK(a.loudness_range_lu() == b.loudness_range_lu());
    CHECK(a.true_peak_max_dbtp() == b.true_peak_max_dbtp());
    CHECK(a.display_peak(1) == b.display_peak(1));
    return 0;
}

int test_reset_and_no_allocation() {
    const double fs = 48000.0;
    TrackMeter m;
    CHECK(m.prepare(fs, 256, 2));
    const Chans loud = sine(fs, 3.0, 1000.0, -10.0);
    const Chans quiet = sine(fs, 3.0, 1000.0, -30.0);
    std::vector<const float*> ptr(2);
    const long before = g_allocations.load();
    float sink = 0.0f;
    auto run = [&](const Chans& x) {
        for (std::size_t pos = 0; pos + 256 <= x[0].size(); pos += 256) {
            ptr[0] = &x[0][pos];
            ptr[1] = &x[1][pos];
            m.process(ptr.data(), 256);
            sink += m.take_peak(0) + m.take_true_peak(1) + m.display_peak(0) +
                    m.momentary_lufs() + m.integrated_lufs() + m.loudness_range_lu();
        }
    };
    run(loud);
    const float i_loud = m.integrated_lufs();
    m.request_reset();
    run(quiet);
    const long allocs = g_allocations.load() - before;
    std::printf("  integrated %.3f, after reset and -30 dBFS %.3f; %ld allocations (sink %g)\n",
                i_loud, m.integrated_lufs(), allocs, static_cast<double>(sink));
    CHECK(allocs == 0);
    CHECK(std::fabs(i_loud + 10.0f) <= 0.15f);
    CHECK(std::fabs(m.integrated_lufs() + 30.0f) <= 0.15f);
    CHECK(m.true_peak_max_dbtp() < -29.0f);
    return 0;
}

int test_denormals() {
    // Holds FTZ for the call and gives the caller's state back.
    TrackMeter m;
    CHECK(m.prepare(48000.0, 128, 2));
    const bool was = flush_to_zero_active();
    const Chans x = sine(48000.0, 0.1, 1000.0, -6.0);
    feed(m, x);
    CHECK(flush_to_zero_active() == was);
    if (SONORA_SANITIZED) return 0;
    // A tone, then the decay into silence: the silent blocks cost no more
    // than the active ones (a denormal stall costs 10-100x).
    const double fs = 48000.0;
    const Chans tone = sine(fs, 5.0, 40.0, -1.0);
    const Chans silence(2, std::vector<float>(static_cast<std::size_t>(fs * 5.0), 0.0f));
    auto time_of = [&](const Chans& c) {
        const auto t0 = std::chrono::steady_clock::now();
        feed(m, c);
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    // The best of three tone-then-silence pairs: a single pair read a
    // neighbour's burst as a stall. A real denormal stall is in all three.
    double a = 1e9, s = 1e9;
    for (int rep = 0; rep < 3; ++rep) {
        a = std::min(a, time_of(tone));
        s = std::min(s, time_of(silence));
    }
    std::printf("  5 s active %.1f ms, 5 s decaying silence %.1f ms (best of 3)\n", a * 1e3, s * 1e3);
    CHECK_LE(s, 2.0 * a + 0.005, "silence time, s");
    return 0;
}

// The code-path figure of one configuration: per block, the minimum over
// the renders (ADR-0011 Amendment 1, item 4).
template <typename Make>
std::vector<double> code_path(std::size_t blocks, int renders, Make&& make,
                              std::vector<double>* first = nullptr) {
    std::vector<double> best(blocks, 1e9);
    if (first != nullptr) first->assign(blocks, 0.0);
    for (int r = 0; r < renders; ++r) {
        auto block = make();  // a fresh instance per render
        for (std::size_t b = 0; b < blocks; ++b) {
            const auto t0 = std::chrono::steady_clock::now();
            block(b);
            const double us =
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0)
                    .count();
            best[b] = std::min(best[b], us);
            if (r == 0 && first != nullptr) (*first)[b] = us;
        }
    }
    return best;
}

// Two code paths timed side by side: block b of A and block b of B back to
// back, the order alternating, so a load that comes and goes over the run
// lands on both and their ratio measures the code. Timed one after the
// other, a neighbour busy during only one of them moved the true-peak ratio
// from 1.8 to 0.8 (local CI, 2026-11-05). Per block, the minimum over the
// renders, as code_path().
template <typename MakeA, typename MakeB>
void code_path_pair(std::size_t blocks, int renders, MakeA&& make_a, MakeB&& make_b,
                    std::vector<double>& best_a, std::vector<double>& best_b,
                    std::vector<double>& first_a) {
    best_a.assign(blocks, 1e9);
    best_b.assign(blocks, 1e9);
    first_a.assign(blocks, 0.0);
    const auto timed = [](auto& block, std::size_t b) {
        const auto t0 = std::chrono::steady_clock::now();
        block(b);
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0)
            .count();
    };
    for (int r = 0; r < renders; ++r) {
        auto a = make_a();  // fresh instances per render
        auto bl = make_b();
        for (std::size_t b = 0; b < blocks; ++b) {
            double ua, ub;
            if ((b + static_cast<std::size_t>(r)) % 2 == 0) {
                ua = timed(a, b);
                ub = timed(bl, b);
            } else {
                ub = timed(bl, b);
                ua = timed(a, b);
            }
            best_a[b] = std::min(best_a[b], ua);
            best_b[b] = std::min(best_b[b], ub);
            if (r == 0) first_a[b] = ua;
        }
    }
}

double percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(p * (v.size() - 1))];
}

int report_cpu() {
    // ADR-0011 §3.5 at q128, 48 kHz, stereo. The class bar is reported, not
    // asserted (Amendment 1, item 4: a shared runner says nothing about the
    // reference machine); the bars held are ratios measured side by side,
    // which a runner's speed does not move.
    const double fs = 48000.0;
    const double seconds = dsp_quick() ? 20.0 : 60.0;
    const int renders = dsp_quick() ? 3 : 5;
    const Chans x = programme(fs, seconds);
    const std::size_t blocks = x[0].size() / 128;
    struct Mode {
        const char* name;
        bool loudness, true_peak;
    };
    const Mode modes[] = {{"peak only", false, false},
                          {"peak + loudness", true, false},
                          {"peak + true-peak", false, true},
                          {"all readings", true, true}};
    const auto make_meter = [&](const TrackMeter::Options& o,
                                std::vector<std::unique_ptr<TrackMeter>>& keep) {
        keep.emplace_back(new TrackMeter);
        TrackMeter* m = keep.back().get();
        m->prepare(fs, 128, 2, o);
        return [m, &x](std::size_t b) {
            const float* p[2] = {&x[0][b * 128], &x[1][b * 128]};
            m->process(p, 128);
        };
    };
    const auto report = [&](const char* name, const std::vector<double>& best,
                            const std::vector<double>& first) {
        std::printf("  CPU q128 48k stereo, %-16s %g s x %d: code path p99.9 %6.2f us max %6.2f "
                    "us; single run p99.9 %6.2f us max %6.2f us\n",
                    name, seconds, renders, percentile(best, 0.999),
                    *std::max_element(best.begin(), best.end()), percentile(first, 0.999),
                    *std::max_element(first.begin(), first.end()));
    };
    std::vector<std::unique_ptr<TrackMeter>> keep;
    for (const Mode& md : modes) {
        if (md.loudness && md.true_peak) continue;  // "all readings": below, beside the interpolator
        TrackMeter::Options o;
        o.loudness = md.loudness;
        o.true_peak = md.true_peak;
        std::vector<double> first;
        const std::vector<double> best =
            code_path(blocks, renders, [&] { return make_meter(o, keep); }, &first);
        report(md.name, best, first);
        keep.clear();
    }
    // The whole meter and the per-sample interpolator over the same blocks,
    // timed side by side, for the block mode's saving.
    TrackMeter::Options o_all;
    o_all.loudness = true;
    o_all.true_peak = true;
    std::vector<std::unique_ptr<TruePeakDetector>> keep_tp;
    std::vector<double> all, per_sample, first_all;
    code_path_pair(
        blocks, renders, [&] { return make_meter(o_all, keep); },
        [&] {
            keep_tp.emplace_back(new TruePeakDetector);
            TruePeakDetector* d = keep_tp.back().get();
            d->prepare(2);
            return [d, &x](std::size_t b) {
                double m = 0.0;
                for (std::uint32_t c = 0; c < 2; ++c) {
                    for (std::size_t k = b * 128; k < b * 128 + 128; ++k) {
                        m = std::max(m, d->process(c, x[c][k]));
                    }
                }
                g_sink = g_sink + m;
            };
        },
        all, per_sample, first_all);
    report(modes[3].name, all, first_all);
    const double med_all = percentile(all, 0.5);
    const double ratio_spike = percentile(all, 0.999) / med_all;
    const double ratio_tp = percentile(per_sample, 0.5) / med_all;
    std::printf("  all readings: median block %.2f us, p99.9 %.1f x the median; the per-sample "
                "true-peak alone costs %.1f x the whole block-mode meter\n",
                med_all, ratio_spike, ratio_tp);
    if (SONORA_SANITIZED) {
        std::printf("  info: ratio bars reported, not held: sanitizer build\n");
        return 0;
    }
    // No block much dearer than the rest: the 100 ms gate step's O(log bins)
    // statistics (one block in 37) and the 10 ms window sums stay inside the
    // noise. p99.9, not the max: one block the host preempted in every
    // render measured 3.4x on a loaded machine, and a per-gate-step cost
    // would still sit far above the 0.1 % tail.
    CHECK_LE(ratio_spike, 3.0, "p99.9 code-path block / median");
    // The block interpolator keeps its saving; the bar leaves room for a
    // compiler that vectorises less.
    CHECK_LE(1.5, ratio_tp, "per-sample true-peak / block-mode meter, at least");
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    std::printf("Fenwick statistics against the bin scan and the exact definitions\n");
    failed += test_fenwick_against_scan_and_exact(dsp_quick() ? 60.0 : 300.0);
    std::printf("Track meter\n");
    failed += test_readings_match_loudness_meter();
    failed += test_peak_ballistics();
    failed += test_true_peak();
    failed += test_true_peak_block_equals_per_sample();
    failed += test_concurrent_reader_misses_no_peak();
    failed += test_nonfinite_input();
    failed += test_long_call_equals_pieces();
    failed += test_reset_and_no_allocation();
    failed += test_denormals();
    failed += report_cpu();
    if (failed != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("dsp meter: all passed\n");
    return 0;
}
