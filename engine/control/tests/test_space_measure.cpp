// SPDX-License-Identifier: GPL-3.0-or-later
//
// The reference-space measurement (ADR-0045 8) on signals built here with a
// known RT60, pre-delay, echo time and level, direct-to-reverberant ratio and
// tempo. Tolerances: RT60 10 %, pre-delay 4 ms, echo 10 ms, levels 2-3 dB,
// tempo 2 bpm. The RT60 and tempo tests also run at 44.1 kHz.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "sonora/space_measure.h"

using namespace sonora;

#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
            return 1;                                                         \
        }                                                                     \
    } while (0)

namespace {

struct Noise {
    std::uint32_t s = 12345;
    float next() {   // uniform -1..1, zero mean
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(static_cast<std::int32_t>(s)) / 2147483648.0f;
    }
};

// Noise bursts 0.25 s long, every 3 s, each followed by a tail that falls
// 60 dB in `rt60` seconds (Polack's model of a late tail: exponentially
// decaying noise).
std::vector<float> decays(double fs, double rt60, int notes) {
    const std::size_t n = std::size_t(fs * 3.0 * notes + fs);
    std::vector<float> x(n, 0.0f);
    Noise ns;
    const std::size_t burst = std::size_t(fs * 0.25);
    for (int k = 0; k < notes; ++k) {
        const std::size_t at = std::size_t(fs * (0.5 + 3.0 * k));
        for (std::size_t i = 0; i < burst; ++i) x[at + i] = 0.5f * ns.next();
        for (std::size_t i = 0; at + burst + i < at + std::size_t(fs * 3.0) - std::size_t(fs * 0.5);
             ++i) {
            const double t = double(i) / fs;
            x[at + burst + i] = float(0.5 * std::pow(10.0, -3.0 * t / rt60)) * ns.next();
        }
    }
    return x;
}

// A one-sample click every 4 s; the tail starts `predelay` after it, `drr`
// dB under the click in energy, falling 60 dB in `rt60`. Returns the exact
// DRR through `truth`.
std::vector<float> clicks(double fs, double predelay, double rt60, double tail_gain,
                          double& truth_drr) {
    const std::size_t n = std::size_t(fs * 20.0);
    std::vector<float> x(n, 0.0f);
    Noise ns;
    double ed = 0, er = 0;
    for (int k = 0; k < 4; ++k) {
        const std::size_t at = std::size_t(fs * (1.0 + 4.0 * k));
        x[at] = 1.0f;
        ed += 1.0;
        const std::size_t p = std::size_t(fs * predelay);
        for (std::size_t i = 0; i < std::size_t(fs * 3.0); ++i) {
            const float v = float(tail_gain * std::pow(10.0, -3.0 * double(i) / fs / rt60)) * ns.next();
            x[at + p + i] += v;
            er += double(v) * v;
        }
    }
    truth_drr = 10.0 * std::log10(ed / er);
    return x;
}

// Noise ticks (5 ms) at irregular times, each repeated at `echo` s with
// gains g, g^2 (a feedback echo).
std::vector<float> echoes(double fs, double echo, double g) {
    const std::size_t n = std::size_t(fs * 16.0);
    std::vector<float> x(n, 0.0f);
    Noise ns;
    const double times[] = {0.4, 2.0, 3.1, 5.3, 6.4, 8.9, 10.5, 12.6};
    for (double t : times)
        for (int rep = 0; rep < 3; ++rep) {
            const std::size_t at = std::size_t(fs * (t + echo * rep));
            const float gain = float(std::pow(g, rep));
            for (std::size_t i = 0; i < std::size_t(fs * 0.005) && at + i < n; ++i)
                x[at + i] += 0.6f * gain * ns.next();
        }
    return x;
}

std::vector<float> pulses(double fs, double bpm) {
    const std::size_t n = std::size_t(fs * 16.0);
    std::vector<float> x(n, 0.0f);
    Noise ns;
    const double period = 60.0 / bpm;
    for (double t = 0.3; t < 15.0; t += period) {
        const std::size_t at = std::size_t(fs * t);
        for (std::size_t i = 0; i < std::size_t(fs * 0.03) && at + i < n; ++i)
            x[at + i] = 0.6f * ns.next() * float(std::exp(-double(i) / (fs * 0.01)));
    }
    return x;
}

int test_rt60() {
    for (double fs : {48000.0, 44100.0})
        for (double rt : {0.6, 1.2, 2.0}) {
            const std::vector<float> x = decays(fs, rt, 4);
            SpaceMeasure m;
            std::string err;
            CHECK(measure_space(x.data(), x.size(), 1, fs, m, err));
            std::fprintf(stderr, "rt60 true %.2f measured %.3f (edt %.3f, %d decays) at %.0f\n", rt,
                         m.rt60_s, m.edt_s, m.decay_count, fs);
            CHECK(m.decay_found && m.decay_count == 4);
            CHECK(std::fabs(m.rt60_s - rt) < 0.10 * rt);
            CHECK(std::fabs(m.edt_s - rt) < 0.15 * rt);
        }
    return 0;
}

int test_predelay_and_drr() {
    for (double pd : {0.020, 0.045, 0.090}) {
        double truth = 0;
        const std::vector<float> x = clicks(48000.0, pd, 1.0, 0.05, truth);
        SpaceMeasure m;
        std::string err;
        CHECK(measure_space(x.data(), x.size(), 1, 48000.0, m, err));
        std::fprintf(stderr, "predelay true %.0f ms measured %.1f; drr true %.2f measured %.2f\n",
                     pd * 1000, m.predelay_ms, truth, m.drr_db);
        CHECK(m.predelay_found && m.drr_found);
        CHECK(std::fabs(m.predelay_ms - pd * 1000.0) < 4.0);
        CHECK(std::fabs(m.drr_db - truth) < 2.0);
        CHECK(std::fabs(m.reverb_wet_db - std::fmin(0.0, -m.drr_db)) < 1e-9);
    }
    return 0;
}

int test_echo() {
    for (double fs : {48000.0, 44100.0})
        for (double echo : {0.375, 0.5625}) {
            const std::vector<float> x = echoes(fs, echo, 0.5);
            SpaceMeasure m;
            std::string err;
            CHECK(measure_space(x.data(), x.size(), 1, fs, m, err));
            std::fprintf(stderr, "echo true %.0f ms measured %.1f (strength %.2f), level %.1f dB\n",
                         echo * 1000, m.echo_ms, m.echo_strength, m.delay_wet_db);
            CHECK(m.echo_found);
            CHECK(std::fabs(m.echo_ms - echo * 1000.0) < 10.0);
            CHECK(m.echo_level_found);
            CHECK(std::fabs(m.delay_wet_db - (-6.02)) < 3.0);
        }
    return 0;
}

int test_tempo() {
    for (double fs : {48000.0, 44100.0})
        for (double bpm : {96.0, 120.0, 140.0}) {
            const std::vector<float> x = pulses(fs, bpm);
            SpaceMeasure m;
            std::string err;
            CHECK(measure_space(x.data(), x.size(), 1, fs, m, err));
            std::fprintf(stderr, "tempo true %.0f measured %.2f\n", bpm, m.tempo_bpm);
            CHECK(m.tempo_found && std::fabs(m.tempo_bpm - bpm) < 2.0);
        }
    return 0;
}

int test_nothing_found_keeps_defaults() {
    // Steady noise: no decay, no transient, no echo. The defaults stay.
    std::vector<float> x(48000 * 12);
    Noise ns;
    for (float& v : x) v = 0.2f * ns.next();
    SpaceMeasure m;
    std::string err;
    CHECK(measure_space(x.data(), x.size(), 1, 48000.0, m, err));
    CHECK(!m.decay_found && !m.predelay_found && !m.echo_found);
    const ReverbDelayReference def, got = to_reference(m);
    CHECK(got.decay_s == def.decay_s && got.predelay_ms == def.predelay_ms &&
          got.delay_ms == def.delay_ms && got.reverb_wet_db == def.reverb_wet_db);
    CHECK(describe_space(m).find("the default stays") != std::string::npos);
    return 0;
}

int test_reference_fill_and_errors() {
    const std::vector<float> x = decays(48000.0, 1.2, 4);
    SpaceMeasure m;
    std::string err;
    CHECK(measure_space(x.data(), x.size(), 1, 48000.0, m, err));
    ReverbDelayReference base;
    base.decay_s = 9.0;
    const ReverbDelayReference got = to_reference(m, base);
    CHECK(std::fabs(got.decay_s - 1.2) < 0.12);
    // Stereo: a left-only copy measures the same as the mono mix of it.
    std::vector<float> st(x.size() * 2);
    for (std::size_t i = 0; i < x.size(); ++i) st[2 * i] = st[2 * i + 1] = x[i];
    SpaceMeasure s;
    CHECK(measure_space(st.data(), x.size(), 2, 48000.0, s, err));
    CHECK(std::fabs(s.rt60_s - m.rt60_s) < 1e-9);
    // Silence and too-short audio are refused with a reason.
    std::vector<float> zero(48000 * 3, 0.0f);
    CHECK(!measure_space(zero.data(), zero.size(), 1, 48000.0, m, err) && !err.empty());
    CHECK(!measure_space(x.data(), 1000, 1, 48000.0, m, err));
    CHECK(!measure_space_file("reference.flac", m, err) && err.find("WAV") != std::string::npos);
    CHECK(!measure_space_file("no_such_reference.wav", m, err));
    return 0;
}

}  // namespace

int main() {
    if (test_rt60() != 0) return 1;
    if (test_predelay_and_drr() != 0) return 1;
    if (test_echo() != 0) return 1;
    if (test_tempo() != 0) return 1;
    if (test_nothing_found_keeps_defaults() != 0) return 1;
    if (test_reference_fill_and_errors() != 0) return 1;
    std::printf("space_measure: ok\n");
    return 0;
}
