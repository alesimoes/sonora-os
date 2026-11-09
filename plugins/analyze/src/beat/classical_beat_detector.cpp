// SPDX-License-Identifier: GPL-3.0-or-later
#include "beat/classical_beat_detector.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "beat/onset_envelope.h"
#include "beat/tempo_track.h"
#include "key/decimator.h"

namespace sonora::analyze {

namespace {

constexpr double kSaturatingAutocorrelation = 0.4;

// Mono mix at kRate, read in blocks; false on cancel or a read error, with
// `error` set.
bool decimated_mono(AudioSource& source, const RunOptions& run, std::vector<float>& sig, std::string& error) {
    const std::uint32_t chs = source.channels();
    const std::int64_t total = source.frames();
    key::Decimator dec;
    if (!dec.prepare(source.sample_rate(), beat::kRate)) {
        error = "beat: cannot prepare the analysis";
        return false;
    }
    const std::int64_t block = std::max<std::int64_t>(run.block_frames, 1024);
    std::vector<float> inter(static_cast<std::size_t>(block) * chs), mono(static_cast<std::size_t>(block));
    for (std::int64_t pos = 0; pos < total;) {
        if (run.cancel && run.cancel->load()) {
            error = "cancelled";
            return false;
        }
        const std::int64_t got = source.read(pos, std::min(block, total - pos), inter.data());
        if (got < 0) {
            error = "beat: read error";
            return false;
        }
        if (got == 0) break;
        for (std::int64_t i = 0; i < got; ++i) {
            double s = 0.0;
            for (std::uint32_t c = 0; c < chs; ++c) s += inter[static_cast<std::size_t>(i) * chs + c];
            mono[static_cast<std::size_t>(i)] = static_cast<float>(s / chs);
        }
        pos += got;
        dec.process(mono.data(), static_cast<std::size_t>(got), sig);
        if (run.progress) run.progress(0.4 * static_cast<double>(pos) / static_cast<double>(total));
    }
    return true;
}

// Sample index (at kRate) where the onset of the flux peak at `frame` starts.
// The flux peaks once the attack has entered the window, so the attack lies in
// its second half; the first sample over a quarter of that range's maximum.
double refine_onset(const std::vector<float>& sig, int frame) {
    const int start = frame * beat::kHop;
    const int lo = std::max(0, start + beat::kWindow / 8);
    const int hi = std::min(static_cast<int>(sig.size()) - 1, start + beat::kWindow);
    float peak = 0.0f;
    for (int i = lo; i <= hi; ++i) peak = std::max(peak, std::fabs(sig[static_cast<std::size_t>(i)]));
    for (int i = lo; i <= hi; ++i)
        if (std::fabs(sig[static_cast<std::size_t>(i)]) > 0.25f * peak) return i;
    return start;
}

struct Refined {
    std::vector<int> frames;
    std::vector<double> samples;  // at kRate
};

Refined refine_all(const std::vector<float>& sig, const std::vector<int>& peaks) {
    Refined r;
    r.frames = peaks;
    for (int p : peaks) r.samples.push_back(refine_onset(sig, p));
    return r;
}

double median_bias(const Refined& r) {
    std::vector<double> d;
    for (std::size_t i = 0; i < r.frames.size(); ++i) d.push_back(r.samples[i] - r.frames[i] * beat::kHop);
    if (d.empty()) return beat::kWindow * 0.5;
    std::nth_element(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(d.size() / 2), d.end());
    return d[d.size() / 2];
}

// Source-rate position of a sample index at kRate.
std::int64_t to_source(double sample, double rate, std::int64_t frames) {
    const auto f = static_cast<std::int64_t>(std::llround(sample * rate / beat::kRate));
    return std::clamp<std::int64_t>(f, 0, frames - 1);
}

std::vector<std::int64_t> increasing(std::vector<std::int64_t> v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

// A beat takes the refined position of an onset within 2 frames, else the
// frame's time plus the median bias.
std::vector<double> beat_samples(const std::vector<int>& beats, const Refined& onsets, double bias) {
    std::vector<double> out;
    for (int b : beats) {
        double at = b * beat::kHop + bias;
        int nearest = 3;
        for (std::size_t i = 0; i < onsets.frames.size(); ++i) {
            const int d = std::abs(onsets.frames[i] - b);
            if (d < nearest) {
                nearest = d;
                at = onsets.samples[i];
            }
        }
        out.push_back(at);
    }
    return out;
}

}  // namespace

bool ClassicalBeatDetector::detect(AudioSource& source, const RunOptions& run, BeatResult& out, std::string& error) {
    const std::int64_t total = source.frames();
    const double rate = source.sample_rate();
    if (source.channels() == 0 || !(rate > 0.0) || total <= 0) {
        error = "beat: the source is empty";
        return false;
    }
    if (static_cast<double>(total) < kMinSeconds * rate) {
        error = "beat: shorter than 4 s";
        return false;
    }
    std::vector<float> sig;
    if (!decimated_mono(source, run, sig, error)) return false;
    float peak = 0.0f;
    for (float v : sig) peak = std::max(peak, std::fabs(v));
    if (!(peak > kSilenceAmplitude)) {
        error = "beat: silent (nothing above -80 dBFS)";
        return false;
    }
    std::vector<float> flux, onset;
    if (!beat::spectral_flux(sig, run.cancel, flux)) {
        error = "cancelled";
        return false;
    }
    if (run.progress) run.progress(0.7);
    if (!beat::adaptive_onset(flux, onset)) {
        error = "beat: no onsets (a constant signal)";
        return false;
    }
    const beat::TempoEstimate tempo = beat::estimate_tempo(onset);
    if (!tempo.found) {
        error = "beat: no periodicity in 60..200 bpm";
        return false;
    }
    if (run.cancel && run.cancel->load()) {
        error = "cancelled";
        return false;
    }
    const Refined onsets = refine_all(sig, beat::pick_peaks(onset));
    const double bias = median_bias(onsets);
    const std::vector<int> frames = beat::track_beats(onset, tempo.bpm);
    const std::vector<double> samples = beat_samples(frames, onsets, bias);

    BeatResult r;
    r.sample_rate = static_cast<std::uint32_t>(std::lround(rate));
    r.frames = total;
    for (double s : onsets.samples) r.onsets.push_back(to_source(s, rate, total));
    r.onsets = increasing(std::move(r.onsets));
    for (double s : samples) r.beats.push_back(to_source(s, rate, total));
    r.beats = increasing(std::move(r.beats));

    r.confidence = std::clamp(beat::beat_strength(onset, frames) * std::min(1.0, tempo.strength / kSaturatingAutocorrelation), 0.0, 1.0);
    r.bpm = tempo.bpm;
    if (r.confidence >= kBeatMinConfidence && r.beats.size() >= 2) {
        std::vector<double> at;
        for (std::int64_t b : r.beats) at.push_back(static_cast<double>(b));
        const double period = beat::fitted_period(at);
        if (period > 0.0) r.bpm = 60.0 * rate / period;
        r.stable = beat::tempo_is_stable(at);
    } else {
        r.beats.clear();
    }
    r.bpm = std::clamp(r.bpm, kBeatMinBpm, kBeatMaxBpm);
    r.stable = r.stable && !r.beats.empty();
    r.method = BeatMethod::spectral_flux;
    if (run.progress) run.progress(1.0);
    out = std::move(r);
    return true;
}

}  // namespace sonora::analyze
