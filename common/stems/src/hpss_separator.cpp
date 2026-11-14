// Harmonic/percussive separation by median filtering (Fitzgerald, "Harmonic/
// Percussive Separation using Median Filtering", DAFx 2010) with soft Wiener
// masks Mh + Mp = 1, so harmonic + percussive reconstructs the input.
#include <algorithm>
#include <complex>
#include <cstddef>

#include "sonora/stems/separators.h"
#include "stft_util.h"

namespace sonora::stems {
namespace {
using namespace detail;
constexpr int kMedian = 17;  // odd; ~0.2 s in time, ~190 Hz at 44.1k in frequency

float median(float* v, int n) {
    std::nth_element(v, v + n / 2, v + n);
    return v[n / 2];
}
}  // namespace

bool HpssSeparator::supports(StemKind k) const {
    return k == StemKind::Harmonic || k == StemKind::Percussive;
}

bool HpssSeparator::separate(const StemRequest& r, StemResult& out, ProgressSink* p,
                             std::string* err) {
    auto fail = [&](const char* m) {
        if (err) *err = m;
        out.stems.clear();
        return false;
    };
    if (!valid_request(r)) return fail("bad request");
    const std::int64_t F = stft_frames(r.frames);
    const std::size_t cells = static_cast<std::size_t>(F) * kBins;
    const int C = r.channels;
    // Progress: pass 1 spectrogram, pass 2 medians, pass 3 per-channel resynthesis.
    const double total = F * (2.0 + C);
    double done = 0;
    auto tick = [&] { return !p || p->report(std::min(1.0, ++done / total)); };

    std::vector<float> mag(cells), harm(cells), perc(cells);
    std::vector<double> acc_h(r.frames), acc_p(r.frames);
    std::vector<std::complex<double>> bins(kBins), bh(kBins), bp(kBins);
    std::vector<float> mono(static_cast<std::size_t>(r.frames), 0.0f);
    out.channels = C;
    out.frames = r.frames;
    out.stems.assign(kStemKindCount, {});
    auto& H = out.stems[static_cast<int>(StemKind::Harmonic)];
    auto& P = out.stems[static_cast<int>(StemKind::Percussive)];
    H.resize(std::size_t(C) * r.frames);
    P.resize(std::size_t(C) * r.frames);

    for (std::int64_t i = 0; i < r.frames; ++i) {
        float s = 0;
        for (int c = 0; c < C; ++c) s += r.in[c][i];
        mono[i] = s / C;
    }
    Stft stft;
    for (std::int64_t f = 0; f < F; ++f) {
        stft.analyze(mono.data(), r.frames, f, bins.data());
        for (int k = 0; k < kBins; ++k) mag[f * kBins + k] = float(std::abs(bins[k]));
        if (!tick()) return fail("cancelled");
    }
    float win[kMedian];
    const int h = kMedian / 2;
    for (std::int64_t f = 0; f < F; ++f) {
        for (int k = 0; k < kBins; ++k) {
            int n = 0;
            for (std::int64_t g = f - h; g <= f + h; ++g)
                win[n++] = mag[std::clamp<std::int64_t>(g, 0, F - 1) * kBins + k];
            harm[f * kBins + k] = median(win, n);
            n = 0;
            for (int j = k - h; j <= k + h; ++j)
                win[n++] = mag[f * kBins + std::clamp(j, 0, kBins - 1)];
            perc[f * kBins + k] = median(win, n);
        }
        if (!tick()) return fail("cancelled");
    }
    // Wiener mask (power 2) for harmonic, stored over harm; percussive = 1 - it.
    for (std::size_t i = 0; i < cells; ++i) {
        float a = harm[i] * harm[i], b = perc[i] * perc[i];
        harm[i] = (a + b > 1e-20f) ? a / (a + b) : 0.5f;
    }
    for (int c = 0; c < C; ++c) {
        std::fill(acc_h.begin(), acc_h.end(), 0.0);
        std::fill(acc_p.begin(), acc_p.end(), 0.0);
        for (std::int64_t f = 0; f < F; ++f) {
            stft.analyze(r.in[c], r.frames, f, bins.data());
            for (int k = 0; k < kBins; ++k) {
                double m = harm[f * kBins + k];
                bh[k] = bins[k] * m;
                bp[k] = bins[k] * (1.0 - m);
            }
            stft.synthesize(bh.data(), r.frames, f, acc_h.data());
            stft.synthesize(bp.data(), r.frames, f, acc_p.data());
            if (!tick()) return fail("cancelled");
        }
        for (std::int64_t i = 0; i < r.frames; ++i) {
            H[c * r.frames + i] = float(acc_h[i]);
            P[c * r.frames + i] = float(acc_p[i]);
        }
    }
    return true;
}

}  // namespace sonora::stems
