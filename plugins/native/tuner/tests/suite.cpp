// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tuner against its bars (quality.json).
// - The detector alone: period accuracy in cents on harmonic tones from 100
//   to 900 Hz at three rates, in noise, with a missing fundamental; and what
//   it does with noise, silence and a tone below the gate.
// - The chooser alone: every key and scale against a table of intervals
//   written here, independently of the masks the processor uses.
// - The processor: the pitch it puts out against the note asked for (hard,
//   half amount, the natural speed's time constant), at every scale; exact
//   transparency where nothing is corrected (amount 0, noise, silence).
// - The shifter alone: the spectral envelope of a two-formant vowel kept
//   where it was while the pitch moves, and the level kept.
// - Hot input, and the common checks (latency in every range, non-finite,
//   reset, denormals, zipper, golden files, CPU).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "sonora/dsp/math.h"
#include "sonora/measure/measure.h"
#include "sonora/native/tuner.h"
#include "sonora/native/tuner_pitch.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

std::string fmt(const char* f, double a, double b = 0.0, double c = 0.0) {
    char buf[240];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

std::string hz(double fs) { return std::to_string(static_cast<int>(fs)) + " Hz"; }

double cents(double a, double b) { return 1200.0 * std::log2(a / b); }

const std::vector<double>& rates() {
    static const std::vector<double> all = {44100.0, 48000.0, 96000.0};
    static const std::vector<double> quick = {44100.0, 96000.0};
    return measure::quick() ? quick : all;
}

// A voice-like harmonic tone: harmonics h = first..last with amplitude 1 / h
// and fixed phases, peak `peak`, faded in over 10 ms. The fundamental follows
// f0(t) (Hz), the phase integrated in double.
template <class F0>
std::vector<float> voice(double fs, double seconds, F0&& f0, double peak_dbfs, int first = 1, int last = 24) {
    const auto n = static_cast<std::size_t>(seconds * fs);
    std::vector<float> y(n);
    double phase = 0.0, top = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double f = f0(static_cast<double>(i) / fs);
        phase += dsp::kTwoPi * f / fs;
        double v = 0.0;
        for (int h = first; h <= last; ++h) {
            if (h * f > 0.45 * fs) break;
            v += std::sin(h * phase + 0.7 * h * h) / h;
        }
        const double fade = std::min(1.0, static_cast<double>(i) / (0.01 * fs));
        y[i] = static_cast<float>(v * fade);
        top = std::max(top, std::fabs(v));
    }
    const double g = std::pow(10.0, peak_dbfs / 20.0) / top;
    for (float& v : y) v = static_cast<float>(v * g);
    return y;
}

std::vector<float> voice(double fs, double seconds, double f0, double peak_dbfs, int first = 1, int last = 24) {
    return voice(fs, seconds, [f0](double) { return f0; }, peak_dbfs, first, last);
}

// Frequency of the strongest component near `guess` (within +-100 cents) in
// y[from, from + n): the peak of the Hann-windowed DFT, found by golden-
// section search. Independent of the processor's own detector.
double est_hz(const std::vector<float>& y, std::size_t from, std::size_t n, double fs, double guess) {
    std::vector<double> w(n);
    for (std::size_t i = 0; i < n; ++i) w[i] = (0.5 - 0.5 * std::cos(dsp::kTwoPi * static_cast<double>(i) / static_cast<double>(n))) * y[from + i];
    auto mag = [&](double f) {
        const double d = dsp::kTwoPi * f / fs;
        const double cd = std::cos(d), sd = std::sin(d);
        double c = 1.0, s = 0.0, re = 0.0, im = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            re += w[i] * c;
            im -= w[i] * s;
            const double nc = c * cd - s * sd;
            s = s * cd + c * sd;
            c = nc;
        }
        return re * re + im * im;
    };
    double lo = guess * std::pow(2.0, -100.0 / 1200.0), hi = guess * std::pow(2.0, 100.0 / 1200.0);
    const double phi = 0.6180339887498949;
    double x1 = hi - phi * (hi - lo), x2 = lo + phi * (hi - lo);
    double f1 = mag(x1), f2 = mag(x2);
    for (int it = 0; it < 26; ++it) {
        if (f1 > f2) {
            hi = x2;
            x2 = x1;
            f2 = f1;
            x1 = hi - phi * (hi - lo);
            f1 = mag(x1);
        } else {
            lo = x1;
            x1 = x2;
            f1 = f2;
            x2 = lo + phi * (hi - lo);
            f2 = mag(x2);
        }
    }
    return 0.5 * (lo + hi);
}

double rms_of(const std::vector<float>& y, std::size_t from, std::size_t to) {
    double s = 0.0;
    for (std::size_t i = from; i < to; ++i) s += static_cast<double>(y[i]) * y[i];
    return std::sqrt(s / static_cast<double>(to - from));
}

std::unique_ptr<Processor> measuring(const Suite& s, double fs, std::uint32_t channels, const Config& c) {
    auto p = s.make();
    measure::apply(*p, c);
    p->prepare(fs, 128, channels);
    return p;
}

std::vector<float> run_mono(const Suite& s, double fs, const std::vector<float>& x, const Config& c) {
    auto p = measuring(s, fs, 1, c);
    Chans in(1, x);
    return measure::render(*p, in)[0];
}

double midi_hz(double midi) { return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0); }

// ---- the detector --------------------------------------------------------------------

struct Run {
    int frames = 0, voiced = 0;
    double worst_cents = 0.0;
};

// Feeds x into a detector prepared for 65-1100 Hz and compares each result
// after `from` seconds with the period `truth` (samples; 0: no pitch, only
// count the voiced frames).
Run detect(double fs, const std::vector<float>& x, double truth, double from_s) {
    tuner::YinDetector d;
    d.prepare(fs, 65.0, 1100.0);
    Run r;
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!d.push(x[i]) || static_cast<double>(i) < from_s * fs) continue;
        ++r.frames;
        if (!d.result().voiced) continue;
        ++r.voiced;
        if (truth > 0.0) r.worst_cents = std::max(r.worst_cents, std::fabs(cents(truth, d.result().period)));
    }
    return r;
}

void detector(Suite& s) {
    const double f0s[] = {101.3, 147.7, 233.1, 331.9, 449.3, 661.7, 893.1};
    for (double fs : rates()) {
        double worst = 0.0, worst_noisy = 0.0, worst_f = 0.0;
        int dropouts = 0, frames = 0, noisy_voiced = 0, noisy_frames = 0;
        for (double f0 : f0s) {
            const Run r = detect(fs, voice(fs, 0.7, f0, -12.0), fs / f0, 0.25);
            dropouts += r.frames - r.voiced;
            frames += r.frames;
            if (r.worst_cents > worst) {
                worst = r.worst_cents;
                worst_f = f0;
            }
            // 20 dB signal-to-noise: white noise at a tenth of the voice's RMS.
            std::vector<float> v = voice(fs, 0.7, f0, -12.0);
            const double rms = rms_of(v, 0, v.size());
            const measure::Chans nz = measure::noise(fs, -20.0, 0.7, 31u, 1);
            double nrms = rms_of(nz[0], 0, nz[0].size());
            const double g = 0.1 * rms / nrms;
            for (std::size_t i = 0; i < v.size(); ++i) v[i] += static_cast<float>(g * nz[0][i]);
            const Run rn = detect(fs, v, fs / f0, 0.25);
            noisy_voiced += rn.voiced;
            noisy_frames += rn.frames;
            worst_noisy = std::max(worst_noisy, rn.worst_cents);
        }
        s.check_le("detector, harmonic tones 101-893 Hz, " + hz(fs) + ": worst period error", worst,
                   s.bar("detector.cents"), "cents", fmt("at %.1f Hz", worst_f));
        s.check_le("detector, same tones, " + hz(fs) + ": frames lost", dropouts, s.bar("detector.dropouts"), "frames",
                   fmt("of %.0f", frames));
        s.check_le("detector, 20 dB signal-to-noise, " + hz(fs) + ": worst period error", worst_noisy,
                   s.bar("detector.noisy_cents"), "cents");
        const double nv = static_cast<double>(noisy_voiced) / noisy_frames;
        s.check("detector, 20 dB signal-to-noise, " + hz(fs) + ": voiced fraction", nv, s.bar("detector.noisy_voiced"),
                nv >= s.bar("detector.noisy_voiced"), "", "bar is a minimum");
        // The fundamental is absent: harmonics 2 to 8 of 200.7 Hz. YIN reports the
        // period of the series, not of its lowest component.
        const Run mf = detect(fs, voice(fs, 0.7, 200.7, -12.0, 2, 8), fs / 200.7, 0.25);
        s.check_le("detector, missing fundamental (harmonics 2-8 of 200.7 Hz), " + hz(fs) + ": worst period error",
                   mf.voiced == mf.frames ? mf.worst_cents : 1200.0, s.bar("detector.cents"), "cents");
        // What is not a voice.
        const Run nz = detect(fs, measure::noise(fs, -20.0, 4.0, 11u, 1)[0], 0.0, 0.2);
        const double fv = static_cast<double>(nz.voiced) / nz.frames;
        s.check_le("detector, white noise at -20 dBFS, " + hz(fs) + ": voiced fraction", fv, s.bar("detector.noise_voiced"), "");
        const Run sil = detect(fs, std::vector<float>(static_cast<std::size_t>(fs), 0.0f), 0.0, 0.1);
        const Run quiet = detect(fs, voice(fs, 0.7, 220.0, -85.0), 0.0, 0.1);
        s.check_le("detector, silence and a -85 dBFS tone, " + hz(fs) + ": voiced frames", sil.voiced + quiet.voiced, 0.0, "frames");
    }
}

// ---- the chooser -----------------------------------------------------------------------

// Written out as intervals, apart from the processor's bit masks.
const std::vector<std::vector<int>> kIntervals = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
    {0, 2, 4, 5, 7, 9, 11},
    {0, 2, 3, 5, 7, 8, 10},
    {0, 2, 3, 5, 7, 8, 11},
    {0, 2, 4, 7, 9},
    {0, 3, 5, 7, 10},
};

// The nearest scale note by brute force; ties (equal distance) give -1.
int reference_note(double midi, int tonic, int scale) {
    int best = -1;
    double best_d = 1e9, second_d = 1e9;
    for (int n = static_cast<int>(std::floor(midi)) - 8; n <= static_cast<int>(std::floor(midi)) + 9; ++n) {
        const int rel = ((n - tonic) % 12 + 12) % 12;
        bool in = false;
        for (int d : kIntervals[static_cast<std::size_t>(scale)]) in |= d == rel;
        if (!in) continue;
        const double d = std::fabs(midi - n);
        if (d < best_d) {
            second_d = best_d;
            best_d = d;
            best = n;
        } else if (d < second_d) {
            second_d = d;
        }
    }
    return second_d - best_d < 1e-9 ? -1 : best;
}

// How far `midi` is from the point where the choice flips (semitones).
double margin_to_flip(double midi, int tonic, int scale) {
    int best = -1;
    double best_d = 1e9, second_d = 1e9;
    for (int n = static_cast<int>(std::floor(midi)) - 8; n <= static_cast<int>(std::floor(midi)) + 9; ++n) {
        const int rel = ((n - tonic) % 12 + 12) % 12;
        bool in = false;
        for (int d : kIntervals[static_cast<std::size_t>(scale)]) in |= d == rel;
        if (!in) continue;
        const double d = std::fabs(midi - n);
        if (d < best_d) {
            second_d = best_d;
            best_d = d;
            best = n;
        } else if (d < second_d) {
            second_d = d;
        }
    }
    (void)best;
    return 0.5 * (second_d - best_d);
}

void chooser(Suite& s) {
    int tested = 0, wrong = 0;
    for (int scale = 0; scale < 6; ++scale) {
        for (int tonic = 0; tonic < 12; ++tonic) {
            for (double midi = 36.0; midi < 84.0; midi += 0.0625) {
                const int want = reference_note(midi, tonic, scale);
                if (want < 0) continue;
                ++tested;
                wrong += tuner::nearest_note(midi, tonic, tuner::kScaleMasks[scale], -1, 0.0) != want;
            }
        }
    }
    s.check_le("scale notes, all 6 scales x 12 keys, 36-84 in 1/16 semitone: wrong choices vs the interval table", wrong, 0.0,
               "", fmt("%.0f points", tested));
    // Hysteresis: a voice parked within the margin of the previous note keeps it.
    const int kept = tuner::nearest_note(60.62, 0, tuner::kScaleMasks[0], 60, tuner::kHysteresisSemitones);
    const int moved = tuner::nearest_note(60.9, 0, tuner::kScaleMasks[0], 60, tuner::kHysteresisSemitones);
    s.check("note hysteresis: 60.62 keeps C4, 60.9 moves to C#4", kept * 100 + moved, 6061, kept == 60 && moved == 61, "");
}

// ---- the processor: pitch out ---------------------------------------------------------

Config hard(const Config& more = {}) {
    Config c = {{"range", 0.0f}, {"speed", 0.0f}, {"amount", 100.0f}, {"scale", 0.0f}, {"key", 0.0f}};
    c.insert(c.end(), more.begin(), more.end());
    return c;
}

void tuning(Suite& s) {
    const double bases[] = {110.0, 220.0, 440.0};
    const double detunes[] = {-47.0, -30.0, -8.0, 0.0, 12.0, 29.0, 46.0};
    for (double fs : rates()) {
        double worst = 0.0, worst_f = 0.0, worst_d = 0.0;
        int cases = 0;
        for (double base : bases) {
            for (double det : detunes) {
                if (measure::quick() && static_cast<int>(det) % 2 != 0 && det != 29.0) continue;
                const double f_in = base * std::pow(2.0, det / 1200.0);
                const std::vector<float> y = run_mono(s, fs, voice(fs, 1.2, f_in, -12.0), hard());
                const double f = est_hz(y, static_cast<std::size_t>(0.6 * fs), static_cast<std::size_t>(0.5 * fs), fs, base);
                const double e = std::fabs(cents(f, base));
                ++cases;
                if (e > worst) {
                    worst = e;
                    worst_f = base;
                    worst_d = det;
                }
            }
        }
        s.check_le("hard retune to the nearest semitone, " + hz(fs) + ": worst |output - note|", worst, s.bar("tuning.hard_cents"),
                   "cents", fmt("%.0f cases; at %.0f Hz, input %+.0f cents", cases, worst_f, worst_d));
    }
    const double fs = 48000.0;
    {
        // Half the distance: 30 cents flat comes out 15 flat.
        const double f_in = 220.0 * std::pow(2.0, -30.0 / 1200.0);
        const std::vector<float> y = run_mono(s, fs, voice(fs, 1.2, f_in, -12.0), hard({{"amount", 50.0f}}));
        const double e = cents(est_hz(y, 28800, 24000, fs, 220.0), 220.0);
        s.check_le("amount 50 %, 30 cents flat: output - 15 cents flat", std::fabs(e + 15.0), s.bar("tuning.amount_cents"), "cents",
                   fmt("%+.2f cents", e));
    }
    {
        // Reference 432 Hz: A4 of the key is 432, so 432 Hz is on the note.
        const std::vector<float> y = run_mono(s, fs, voice(fs, 1.2, 432.0 * std::pow(2.0, 25.0 / 1200.0), -12.0),
                                              hard({{"reference", 432.0f}}));
        const double e = cents(est_hz(y, 28800, 24000, fs, 432.0), 432.0);
        s.check_le("reference A4 = 432 Hz, input 25 cents sharp: output - 432 Hz", std::fabs(e), s.bar("tuning.hard_cents"), "cents");
    }
    {
        // The natural speed: the residual error falls exponentially with the
        // time constant asked for, whatever the delay before the correction
        // starts. A line through the log of the error at five instants.
        const double tau = 0.3;
        const double f_in = 220.0 * std::pow(2.0, -30.0 / 1200.0);
        auto p = measuring(s, fs, 1, hard({{"speed", static_cast<float>(tau * 1000.0)}, {"range", 1.0f}}));
        const std::vector<float> x = voice(fs, 1.4, f_in, -12.0);
        const std::vector<float> y = measure::render(*p, Chans(1, x))[0];
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        int n = 0;
        for (double t : {0.15, 0.3, 0.45, 0.6, 0.75}) {
            const auto from = static_cast<std::size_t>((t - 0.04) * fs);
            const double e = -cents(est_hz(y, from, static_cast<std::size_t>(0.08 * fs), fs, 220.0), 220.0);
            if (e <= 0.05) continue;
            sx += t;
            sy += std::log(e);
            sxx += t * t;
            sxy += t * std::log(e);
            ++n;
        }
        const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        const double tau_est = -1.0 / slope;
        s.check_le("natural retune, speed 300 ms, 30 cents flat: time constant of the residual, relative error",
                   std::fabs(tau_est / tau - 1.0), s.bar("tuning.tau_rel"), "", fmt("%.0f ms measured", tau_est * 1000.0));
    }
}

void scales(Suite& s) {
    const double fs = 48000.0;
    const double xs[] = {55.3, 57.7, 59.4, 61.1, 63.8, 66.45};
    const std::vector<int> tonics = measure::quick() ? std::vector<int>{2} : std::vector<int>{0, 6};
    for (int scale = 0; scale < 6; ++scale) {
        double worst = 0.0;
        int cases = 0;
        for (int tonic : tonics) {
            for (double x : xs) {
                const int want = reference_note(x, tonic, scale);
                // Not within 0.2 semitone of where the choice flips: there the
                // detector's own error could legitimately decide.
                if (want < 0 || margin_to_flip(x, tonic, scale) < 0.2) continue;
                const std::vector<float> y = run_mono(s, fs, voice(fs, 0.95, midi_hz(x), -12.0),
                                                      hard({{"scale", static_cast<float>(scale)}, {"key", static_cast<float>(tonic)}}));
                const double f = est_hz(y, static_cast<std::size_t>(0.55 * fs), static_cast<std::size_t>(0.35 * fs), fs, midi_hz(want));
                worst = std::max(worst, std::fabs(cents(f, midi_hz(want))));
                ++cases;
            }
        }
        const char* names[] = {"chromatic", "major", "minor", "harmonic minor", "major pentatonic", "minor pentatonic"};
        s.check_le(std::string("processor snaps to the ") + names[scale] + " scale: worst |output - note|", worst,
                   s.bar("tuning.scale_cents"), "cents", fmt("%.0f cases", cases));
    }
}

// ---- transparency -----------------------------------------------------------------------

void transparency(Suite& s) {
    const double fs = 48000.0;
    auto delayed_diff = [&](const std::vector<float>& x, const std::vector<float>& y, std::uint32_t lat) {
        double worst = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i) {
            const float want = i >= lat ? x[i - lat] : 0.0f;
            worst = std::max(worst, static_cast<double>(std::fabs(y[i] - want)));
        }
        return worst;
    };
    {
        // Amount 0 on a voiced tone: pitch-aligned grains, ratio 1: the output is
        // the input, delayed by the latency, sample for sample.
        auto p = measuring(s, fs, 1, hard({{"amount", 0.0f}, {"range", 1.0f}}));
        const std::vector<float> x = voice(fs, 1.0, 187.3, -6.0);
        const std::vector<float> y = measure::render(*p, Chans(1, x))[0];
        s.check_le("amount 0 %, voiced tone: max |out - input delayed by the latency|", delayed_diff(x, y, p->latency()),
                   s.bar("transparent_abs"), "FS", fmt("latency %.0f samples", p->latency()));
    }
    {
        // Noise has no pitch: the shifter is not engaged, whatever the settings.
        auto p = measuring(s, fs, 1, hard({{"range", 1.0f}}));
        Chans nz = measure::noise(fs, -12.0, 1.0, 5u, 1);
        measure::band_limit_and_fade(nz, fs);
        const std::vector<float> y = measure::render(*p, nz)[0];
        s.check_le("noise in, hard retune: max |out - input delayed by the latency|", delayed_diff(nz[0], y, p->latency()),
                   s.bar("transparent_abs"), "FS");
    }
    {
        auto p = measuring(s, fs, 1, hard());
        const std::vector<float> y = measure::render(*p, Chans(1, std::vector<float>(48000, 0.0f)))[0];
        double top = 0.0;
        for (float v : y) top = std::max(top, static_cast<double>(std::fabs(v)));
        s.check_le("silence in: max |out|", top, 0.0, "FS");
    }
}

// ---- the shifter: the formants stay ------------------------------------------------------

// The spectral envelope of a two-formant vowel (a resonance at 600 Hz and
// one at 1800 Hz, Lorentzian-shaped), linear amplitude.
double envelope(double f) {
    return 1.0 / std::sqrt(1.0 + std::pow((f - 600.0) / 120.0, 2.0)) +
           0.6 / std::sqrt(1.0 + std::pow((f - 1800.0) / 250.0, 2.0)) + 0.02;
}

double amp_at(const std::vector<float>& y, std::size_t from, double fs, double f) {
    const std::size_t n = y.size() - from;
    double re = 0.0, im = 0.0, wsum = 0.0;
    const double w0 = dsp::kTwoPi * f / fs;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(dsp::kTwoPi * static_cast<double>(i) / static_cast<double>(n));
        const double ph = w0 * static_cast<double>(from + i);
        re += w * y[from + i] * std::cos(ph);
        im -= w * y[from + i] * std::sin(ph);
        wsum += w;
    }
    return 2.0 * std::hypot(re, im) / wsum;
}

void formants(Suite& s) {
    const double fs = 48000.0, f0 = 140.0;
    const double ratios_ct[] = {50.0, -50.0, 100.0, -100.0, 400.0};
    for (double ct : ratios_ct) {
        const double ratio = std::pow(2.0, ct / 1200.0);
        // Additive vowel: harmonic h at amplitude envelope(h f0), all in phase,
        // so each cycle is one pulse ringing at the formants, as a glottal pulse is.
        const auto n = static_cast<std::size_t>(1.4 * fs);
        std::vector<float> x(n);
        for (std::size_t i = 0; i < n; ++i) {
            double v = 0.0;
            for (int h = 1; h * f0 < 3600.0; ++h)
                v += envelope(h * f0) * std::cos(dsp::kTwoPi * h * f0 * static_cast<double>(i) / fs);
            x[i] = static_cast<float>(0.12 * v);
        }
        tuner::PsolaShifter sh;
        sh.prepare(fs, 65.0, 1);
        std::vector<float> y(n);
        for (std::size_t i = 0; i < n; ++i) sh.tick(&x[i], &y[i], fs / f0, ratio, true);
        const std::size_t from = static_cast<std::size_t>(0.5 * fs);
        // Harmonic by harmonic: the output's against the envelope at its new
        // frequency, once a common gain is taken out (the median).
        std::vector<double> want_db, moved_db, got_db;
        double top = 0.0;
        for (int h = 1; h * f0 * ratio < 3600.0; ++h) top = std::max(top, envelope(h * f0 * ratio));
        std::vector<int> held;
        for (int h = 1; h * f0 * ratio < 3600.0; ++h) {
            const double f = h * f0 * ratio;
            got_db.push_back(20.0 * std::log10(std::max(amp_at(y, from, fs, f), 1e-12)));
            want_db.push_back(20.0 * std::log10(envelope(f)));
            moved_db.push_back(20.0 * std::log10(envelope(f / ratio)));
            if (envelope(f) >= top * 0.1) held.push_back(h - 1);
        }
        auto offset_of = [&](const std::vector<double>& ref) {
            std::vector<double> d;
            for (int i : held) d.push_back(got_db[static_cast<std::size_t>(i)] - ref[static_cast<std::size_t>(i)]);
            std::sort(d.begin(), d.end());
            return d[d.size() / 2];
        };
        const double g1 = offset_of(want_db), g2 = offset_of(moved_db);
        double w1 = 0.0, w2 = 0.0;
        for (int i : held) {
            w1 = std::max(w1, std::fabs(got_db[static_cast<std::size_t>(i)] - g1 - want_db[static_cast<std::size_t>(i)]));
            w2 = std::max(w2, std::fabs(got_db[static_cast<std::size_t>(i)] - g2 - moved_db[static_cast<std::size_t>(i)]));
        }
        const std::string what = fmt("formants kept, vowel at 140 Hz shifted %+.0f cents: worst harmonic off the original envelope", ct);
        const std::string note = fmt("%.0f harmonics held", static_cast<double>(held.size()));
        if (std::fabs(ct) <= 100.0) s.check_le(what, w1, s.bar("formant.envelope_db"), "dB", note);
        else s.info(what + " (beyond a tuner's usual correction, not barred)", w1, "dB", note);
        s.info(fmt("the same harmonics against the envelope moved with the pitch (what resampling would give), %+.0f cents", ct), w2, "dB");
        // The level of a steady voiced tone through the same shift.
        const std::vector<float> v = voice(fs, 1.4, 187.0, -12.0);
        tuner::PsolaShifter sv;
        sv.prepare(fs, 65.0, 1);
        std::vector<float> yv(v.size());
        for (std::size_t i = 0; i < v.size(); ++i) sv.tick(&v[i], &yv[i], fs / 187.0, ratio, true);
        s.check_le(fmt("level of a voiced tone, shifted %+.0f cents: |output - input| RMS", ct),
                   std::fabs(20.0 * std::log10(rms_of(yv, from, v.size()) / rms_of(v, from, v.size()))), s.bar("level_db"), "dB");
    }
}

// A pure tone, corrected by 30 cents: what the grains add to it, as THD+N
// (the harmonics and the grain seams, relative to the fundamental).
void purity(Suite& s) {
    for (double fs : {48000.0, 96000.0}) {
        const double f_in = 220.0 * std::pow(2.0, -30.0 / 1200.0);
        const auto n = static_cast<std::size_t>(1.5 * fs);
        std::vector<float> x(n);
        for (std::size_t i = 0; i < n; ++i) x[i] = static_cast<float>(0.25 * std::sin(dsp::kTwoPi * f_in * static_cast<double>(i) / fs));
        const std::vector<float> y = run_mono(s, fs, x, hard({{"range", 1.0f}}));
        const double f = est_hz(y, static_cast<std::size_t>(0.8 * fs), static_cast<std::size_t>(0.5 * fs), fs, 220.0);
        s.check_le("pure sine 30 cents flat, hard retune, " + hz(fs) + ": THD+N of the output", measure::thdn_db(y, static_cast<std::size_t>(0.8 * fs), fs, f),
                   s.bar("purity_db"), "dB");
    }
}

// ---- hot input --------------------------------------------------------------------------

void hot_input(Suite& s) {
    const double fs = 48000.0;
    for (double dbfs : {0.0, 12.0, 24.0}) {
        auto p = measuring(s, fs, 2, hard({{"range", 1.0f}}));
        Chans x = measure::silence(fs, 0.5);
        const auto amp = static_cast<float>(std::pow(10.0, dbfs / 20.0));
        for (auto& ch : x) {
            for (std::size_t i = 0; i < ch.size(); ++i) {
                const double ph = std::fmod(223.0 * static_cast<double>(i) / fs, 1.0);
                ch[i] = ph < 0.5 ? amp : -amp;
            }
        }
        const Chans y = measure::render(*p, x);
        bool finite = true;
        double top = 0.0;
        for (const auto& ch : y) {
            for (float v : ch) {
                finite &= std::isfinite(v);
                top = std::max(top, static_cast<double>(std::fabs(v)));
            }
        }
        const double bound = s.bar("hot.max_ratio") * amp;
        s.check(fmt("hot square at %+.0f dBFS, 223 Hz, hard retune: output finite and within the input peak", dbfs), top / amp,
                s.bar("hot.max_ratio"), finite && top <= bound, "x peak");
    }
}

// ---- golden and CPU on voiced signals -------------------------------------------------------

std::vector<float> glide(double fs, double seconds) {
    // 150 to 300 Hz over the length, with 5.5 Hz vibrato of +-30 cents.
    return voice(fs, seconds, [seconds](double t) {
        return 150.0 * std::pow(2.0, t / seconds) * std::pow(2.0, 0.025 * std::sin(dsp::kTwoPi * 5.5 * t));
    }, -12.0);
}

Chans stereo_of(const std::vector<float>& m) {
    Chans x(2, m);
    for (float& v : x[1]) v *= -0.5f;
    return x;
}

void voiced_golden(Suite& s, const Config& cfg) {
    const std::string dir = s.args().golden_dir;
    for (double fs : measure::kRates) {
        auto p = measuring(s, fs, 2, cfg);
        const Chans y = measure::render(*p, stereo_of(glide(fs, 1.0)));
        const std::string path = dir + "/tuner-voiced-" + std::to_string(static_cast<int>(fs)) + ".wav";
        const std::string what = "golden, a voiced glide with vibrato, " + hz(fs) + ": max |out - golden|";
        if (s.args().regenerate_golden) {
            if (!measure::write_wav_f32(path, y, fs)) s.fail("cannot write " + path);
            else s.info(what, 0.0, "abs", "regenerated");
            continue;
        }
        Chans g;
        double gfs = 0.0;
        std::string err;
        if (!measure::read_wav_f32(path, g, gfs, err) || g.size() != y.size() || g[0].size() != y[0].size()) {
            s.fail("cannot read " + path + " or its shape differs: " + err);
            continue;
        }
        double worst = 0.0;
        for (std::size_t c = 0; c < y.size(); ++c)
            for (std::size_t i = 0; i < y[c].size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(y[c][i] - g[c][i])));
        s.check_le(what, worst, s.bar("golden_abs"), "abs");
    }
}

void cpu_voiced(Suite& s, const Config& cfg) {
    if (!s.args().cpu) return;
    const double fs = 48000.0;
    const auto n = static_cast<std::size_t>(s.args().cpu_seconds * fs);
    // A voice gliding through two octaves with deep vibrato, corrected hard at
    // the lowest range: the detector's and the shifter's busiest case.
    const std::vector<float> m = voice(fs, static_cast<double>(n) / fs, [](double t) {
        return 200.0 * std::pow(2.0, 0.5 * std::sin(dsp::kTwoPi * 0.3 * t)) * std::pow(2.0, 0.03 * std::sin(dsp::kTwoPi * 5.5 * t));
    }, -12.0);
    Config c = cfg;
    c.push_back({"range", 0.0f});
    c.push_back({"speed", 0.0f});
    measure::check_cpu_on(s, c, stereo_of(m), ", voiced glide, hard retune, low range");
}

}  // namespace

measure::Config tuner_golden_config() {
    return {{"key", 0.0f}, {"scale", 1.0f}, {"speed", 30.0f}, {"amount", 100.0f}};
}

int tuner(const measure::Args& args) {
    Suite s("tuner", [] { return make_tuner(); }, args);
    detector(s);
    chooser(s);
    tuning(s);
    scales(s);
    transparency(s);
    formants(s);
    purity(s);
    hot_input(s);
    const Config golden = tuner_golden_config();
    // Noise has no pitch, so with the correction off the output is the input
    // delayed by exactly the latency, which the harness can measure.
    std::vector<std::pair<std::string, Config>> latency;
    const char* range_names[] = {"low range", "medium range", "high range"};
    for (int r = 0; r < 3; ++r) latency.push_back({range_names[r], {{"range", static_cast<float>(r)}, {"amount", 0.0f}}});
    measure::check_latency(s, latency);
    measure::check_nonfinite(s, golden);
    measure::check_reset(s, golden);
    measure::check_denormals(s, golden);
    measure::check_zipper(s, {{"range", 0.0f}, {"scale", 0.0f}, {"speed", 100.0f}}, "");
    measure::check_golden(s, golden);
    voiced_golden(s, golden);
    measure::check_cpu(s, golden, "");
    cpu_voiced(s, golden);
    return s.finish();
}

}  // namespace sonora::native::suites
