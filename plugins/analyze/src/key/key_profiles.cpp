// SPDX-License-Identifier: GPL-3.0-or-later
#include "key/key_profiles.h"

#include <algorithm>
#include <cmath>

namespace sonora::analyze::key {

double profile_correlation(const std::array<double, 12>& chroma, const double* profile, int tonic) noexcept {
    double mc = 0.0, mp = 0.0;
    for (int i = 0; i < 12; ++i) {
        mc += chroma[static_cast<std::size_t>(i)];
        mp += profile[i];
    }
    mc /= 12.0;
    mp /= 12.0;
    double cov = 0.0, vc = 0.0, vp = 0.0;
    for (int i = 0; i < 12; ++i) {
        const double c = chroma[static_cast<std::size_t>((i + tonic) % 12)] - mc;
        const double p = profile[i] - mp;
        cov += c * p;
        vc += c * c;
        vp += p * p;
    }
    if (!(vc > 1e-18 * std::max(1.0, mc * mc * 144.0)) || !(vp > 0.0)) return 0.0;
    return cov / std::sqrt(vc * vp);
}

KeyRanking rank_keys(const std::array<double, 12>& chroma) noexcept {
    double r[24];
    for (int t = 0; t < 12; ++t) {
        r[t] = profile_correlation(chroma, kMajorProfile, t);
        r[12 + t] = profile_correlation(chroma, kMinorProfile, t);
    }
    int b = 0;
    for (int i = 1; i < 24; ++i)
        if (r[i] > r[b]) b = i;
    int s = b == 0 ? 1 : 0;
    for (int i = 0; i < 24; ++i)
        if (i != b && r[i] > r[s]) s = i;
    KeyRanking k;
    k.tonic = b % 12;
    k.minor = b >= 12;
    k.best = r[b];
    k.second = r[s];
    const double headroom = 1.0 - k.second;
    k.confidence = headroom > 1e-9 ? std::clamp((k.best - k.second) / headroom, 0.0, 1.0) : 0.0;
    return k;
}

}  // namespace sonora::analyze::key
