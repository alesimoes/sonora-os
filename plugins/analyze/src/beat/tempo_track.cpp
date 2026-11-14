// SPDX-License-Identifier: GPL-3.0-or-later
#include "beat/tempo_track.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "beat/onset_envelope.h"

namespace sonora::analyze::beat {

namespace {
constexpr double kMinBpm = 60.0, kMaxBpm = 200.0;
constexpr double kLambda = 20.0;
constexpr double kStableTolerance = 0.04;

std::size_t uz(int i) { return static_cast<std::size_t>(i); }

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    return v[v.size() / 2];
}

std::vector<double> intervals(const std::vector<double>& pos) {
    std::vector<double> iv;
    for (std::size_t i = 1; i < pos.size(); ++i) iv.push_back(pos[i] - pos[i - 1]);
    return iv;
}

// Autocorrelation of the mean-removed envelope, each lag normalised by its
// overlap and by lag 0, so a perfectly periodic envelope reads ~1 at its period.
std::vector<double> autocorrelation(const std::vector<float>& onset, int max_lag) {
    const std::size_t n = onset.size();
    double mean = 0.0;
    for (float v : onset) mean += v;
    mean /= static_cast<double>(n);
    std::vector<double> x(n);
    double energy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        x[i] = onset[i] - mean;
        energy += x[i] * x[i];
    }
    std::vector<double> ac(uz(max_lag) + 1, 0.0);
    if (!(energy > 0.0)) return ac;
    for (std::size_t l = 0; l < ac.size() && l < n; ++l) {
        double s = 0.0;
        for (std::size_t i = 0; i + l < n; ++i) s += x[i] * x[i + l];
        ac[l] = (s / static_cast<double>(n - l)) / (energy / static_cast<double>(n));
    }
    return ac;
}

bool is_peak(const std::vector<double>& ac, int l) {
    return ac[uz(l)] > ac[uz(l) - 1] && ac[uz(l)] >= ac[uz(l) + 1];
}

double parabolic_lag(const std::vector<double>& ac, int l) {
    const double a = ac[uz(l) - 1], b = ac[uz(l)], c = ac[uz(l) + 1];
    const double den = a - 2.0 * b + c;
    return den < 0.0 ? l + std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : l;
}
}  // namespace

TempoEstimate estimate_tempo(const std::vector<float>& onset) {
    TempoEstimate out;
    const int lmin = static_cast<int>(std::floor(60.0 * kFrameRate / kMaxBpm)) - 1;
    const int lmax = static_cast<int>(std::ceil(60.0 * kFrameRate / kMinBpm)) + 1;
    if (onset.size() < uz(3 * lmax)) return out;
    const std::vector<double> ac = autocorrelation(onset, 2 * lmax + 3);
    double best = 0.0;
    for (int l = lmin + 1; l < lmax; ++l)
        if (is_peak(ac, l)) best = std::max(best, ac[uz(l)]);
    if (!(best > 0.0)) return out;
    int pick = -1;
    for (int l = lmin + 1; l < lmax && pick < 0; ++l)
        if (is_peak(ac, l) && ac[uz(l)] >= 0.9 * best) pick = l;
    double lag = parabolic_lag(ac, pick);
    double strength = ac[uz(pick)];
    if (60.0 * kFrameRate / lag > 160.0) {
        int top = 2 * pick;
        for (int l = 2 * pick - 2; l <= 2 * pick + 2; ++l)
            if (ac[uz(l)] > ac[uz(top)]) top = l;
        if (ac[uz(top)] > 1.15 * strength && is_peak(ac, top)) {
            lag = parabolic_lag(ac, top);
            strength = ac[uz(top)];
        }
    }
    out.found = true;
    out.bpm = 60.0 * kFrameRate / lag;
    out.strength = std::clamp(strength, 0.0, 1.0);
    return out;
}

std::vector<int> track_beats(const std::vector<float>& onset, double bpm) {
    const int n = static_cast<int>(onset.size());
    const double period = 60.0 * kFrameRate / bpm;
    const int lo = static_cast<int>(std::lround(2.0 * period)), hi = static_cast<int>(std::lround(0.5 * period));
    std::vector<double> score(uz(n), 0.0);
    std::vector<int> prev(uz(n), -1);
    for (int t = 0; t < n; ++t) {
        double best = 0.0;
        int from = -1;
        for (int s = std::max(0, t - lo); s <= t - hi; ++s) {
            const double r = std::log((t - s) / period);
            const double v = score[uz(s)] - kLambda * r * r;
            if (v > best) {
                best = v;
                from = s;
            }
        }
        score[uz(t)] = onset[uz(t)] + best;
        prev[uz(t)] = from;
    }
    int end = std::max(0, n - static_cast<int>(std::lround(1.5 * period)));
    for (int t = end; t < n; ++t)
        if (score[uz(t)] > score[uz(end)]) end = t;
    std::vector<int> beats;
    for (int t = end; t >= 0; t = prev[uz(t)]) beats.push_back(t);
    std::reverse(beats.begin(), beats.end());
    return beats;
}

double beat_strength(const std::vector<float>& onset, const std::vector<int>& beats) {
    if (beats.empty()) return 0.0;
    const int n = static_cast<int>(onset.size());
    double sum = 0.0;
    for (int b : beats) {
        float m = 0.0f;
        for (int t = std::max(0, b - 2); t <= std::min(n - 1, b + 2); ++t) m = std::max(m, onset[uz(t)]);
        sum += std::min(1.0f, m);
    }
    return sum / static_cast<double>(beats.size());
}

bool tempo_is_stable(const std::vector<double>& pos) {
    if (pos.size() < 3) return true;
    const std::vector<double> iv = intervals(pos);
    const std::size_t w = std::min<std::size_t>(5, iv.size());
    std::vector<double> local;
    for (std::size_t i = 0; i + w <= iv.size(); ++i) {
        const auto first = iv.begin() + static_cast<std::ptrdiff_t>(i);
        local.push_back(median(std::vector<double>(first, first + static_cast<std::ptrdiff_t>(w))));
    }
    const auto mm = std::minmax_element(local.begin(), local.end());
    return (*mm.second - *mm.first) <= kStableTolerance * median(local);
}

double fitted_period(const std::vector<double>& pos) {
    if (pos.size() < 2) return 0.0;
    const std::vector<double> iv = intervals(pos);
    const double typical = median(iv);
    if (!(typical > 0.0)) return 0.0;
    std::vector<double> idx(pos.size(), 0.0);
    for (std::size_t i = 1; i < pos.size(); ++i) idx[i] = idx[i - 1] + std::max(1.0, std::round(iv[i - 1] / typical));
    double mi = 0.0, mp = 0.0;
    for (std::size_t i = 0; i < pos.size(); ++i) {
        mi += idx[i];
        mp += pos[i];
    }
    mi /= static_cast<double>(pos.size());
    mp /= static_cast<double>(pos.size());
    double num = 0.0, den = 0.0;
    for (std::size_t i = 0; i < pos.size(); ++i) {
        num += (idx[i] - mi) * (pos[i] - mp);
        den += (idx[i] - mi) * (idx[i] - mi);
    }
    return den > 0.0 ? num / den : 0.0;
}

}  // namespace sonora::analyze::beat
