// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

#include "sonora/native/tuner.h"
#include "sonora/native/tuner_pitch.h"
#include "tuner_eval_run.h"

namespace sonora::native::tuner_eval {

const char* const kRangeNames[kRangeCount] = {"low", "medium", "high"};

std::vector<PitchFrame> detect_pitch(const Clip& clip, int range) {
    std::vector<PitchFrame> out;
    if (range < 0 || range >= kRangeCount) return out;
    tuner::YinDetector yin;
    if (!yin.prepare(clip.fs, tuner::kRangeMinHz[range], tuner::kRangeMaxHz[range])) return out;
    // The window is about 2 * tmax + 65 decimated samples and the lag search
    // compares its halves: the pitch belongs to its middle.
    const double dec = std::max(1.0, std::round(clip.fs / 12000.0));
    const double fsd = clip.fs / dec;
    const double delay = (fsd / tuner::kRangeMinHz[range] + 34.5) / fsd;
    for (std::size_t i = 0; i < clip.samples.size(); ++i) {
        if (!yin.push(clip.samples[i])) continue;
        const auto& r = yin.result();
        PitchFrame f;
        f.time_s = static_cast<double>(i + 1) / clip.fs - delay;
        f.voiced = r.voiced && r.period > 0.0;
        f.hz = f.voiced ? clip.fs / r.period : 0.0;
        out.push_back(f);
    }
    return out;
}

std::vector<PitchFrame> resample_to(const std::vector<PitchFrame>& estimate,
                                    const std::vector<PitchFrame>& reference) {
    std::vector<PitchFrame> out;
    out.reserve(reference.size());
    std::size_t j = 0;
    const double hop = estimate.size() > 1 ? estimate[1].time_s - estimate[0].time_s : 0.01;
    for (const PitchFrame& r : reference) {
        while (j + 1 < estimate.size() &&
               std::fabs(estimate[j + 1].time_s - r.time_s) <= std::fabs(estimate[j].time_s - r.time_s))
            ++j;
        PitchFrame f{r.time_s, 0.0, false};
        if (!estimate.empty() && std::fabs(estimate[j].time_s - r.time_s) <= hop) {
            f = estimate[j];
            f.time_s = r.time_s;
        }
        out.push_back(f);
    }
    return out;
}

namespace {

// Normalised autocorrelation over one window: the first lag peak above 0.6,
// parabola-refined (the first, so a multiple of the period is not preferred
// to the period). 0 for silence or an unclear pitch.
double acf_pitch(const float* x, int n, double fs) {
    double e0 = 0.0;
    for (int i = 0; i < n; ++i) e0 += static_cast<double>(x[i]) * x[i];
    if (e0 < 1e-6 * n) return 0.0;  // below about -60 dBFS RMS
    const int lmin = static_cast<int>(fs / 1100.0);
    const int lmax = std::min(n / 2, static_cast<int>(fs / 60.0));
    if (lmin < 2 || lmax <= lmin) return 0.0;
    std::vector<double> r(static_cast<std::size_t>(lmax) + 2, 0.0);
    for (int lag = lmin - 1; lag <= lmax + 1 && lag < n; ++lag) {
        double s = 0.0, e1 = 0.0;
        for (int i = 0; i + lag < n; ++i) {
            s += static_cast<double>(x[i]) * x[i + lag];
            e1 += static_cast<double>(x[i + lag]) * x[i + lag];
        }
        r[static_cast<std::size_t>(lag)] = s / std::sqrt(e0 * e1 + 1e-30);
    }
    for (int lag = lmin; lag <= lmax; ++lag) {
        const std::size_t k = static_cast<std::size_t>(lag);
        const double c = r[k], l = r[k - 1], u = r[k + 1];
        if (c < 0.6 || c < l || c < u) continue;
        const double den = l - 2.0 * c + u;
        const double off = den < -1e-12 ? std::max(-1.0, std::min(1.0, 0.5 * (l - u) / den)) : 0.0;
        return fs / (lag + off);
    }
    return 0.0;
}

}  // namespace

double in_scale_fraction(const std::vector<float>& audio, double fs, int key, int scale,
                         double tol_cents) {
    if (scale < 0 || scale >= 6) return std::numeric_limits<double>::quiet_NaN();
    const std::size_t n = static_cast<std::size_t>(0.04 * fs), hop = static_cast<std::size_t>(0.01 * fs);
    std::size_t voiced = 0, hits = 0;
    for (std::size_t pos = 0; pos + n <= audio.size(); pos += hop) {
        const double hz = acf_pitch(audio.data() + pos, static_cast<int>(n), fs);
        if (!(hz > 0.0)) continue;
        ++voiced;
        const double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
        const int base = static_cast<int>(std::floor(midi));
        double best = 1e9;
        for (int note = base - 1; note <= base + 2; ++note)
            if (tuner::in_scale(note, key, tuner::kScaleMasks[scale])) best = std::min(best, std::fabs(midi - note));
        if (best * 100.0 <= tol_cents) ++hits;
    }
    return detail::ratio(hits, voiced);
}

std::vector<float> run_tuner(const Clip& clip, int key, int scale, int range) {
    std::unique_ptr<Processor> p = make_tuner();
    p->set_param(tuner::kKey, static_cast<float>(key));
    p->set_param(tuner::kScale, static_cast<float>(scale));
    p->set_param(tuner::kSpeed, 0.0f);
    p->set_param(tuner::kAmount, 100.0f);
    p->set_param(tuner::kRange, static_cast<float>(range));
    constexpr std::uint32_t kBlock = 512;
    if (!p->prepare(clip.fs, kBlock, 1)) return {};
    const std::size_t lat = p->latency();
    std::vector<float> in = clip.samples;
    in.resize(in.size() + lat, 0.0f);  // flush the latency
    std::vector<float> out(in.size(), 0.0f);
    const Transport tr;
    for (std::size_t pos = 0; pos < in.size(); pos += kBlock) {
        const std::uint32_t frames = static_cast<std::uint32_t>(std::min<std::size_t>(kBlock, in.size() - pos));
        const float* ip[1] = {in.data() + pos};
        float* op[1] = {out.data() + pos};
        const AudioBuffers io{ip, op, nullptr, 1, 0, frames};
        p->process(io, nullptr, 0, tr);
    }
    return std::vector<float>(out.begin() + static_cast<std::ptrdiff_t>(lat), out.end());
}

}  // namespace sonora::native::tuner_eval
