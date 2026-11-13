// Centre-channel extraction: soft mask on the mid signal from the mid/side
// power ratio per STFT bin. Vocals = centre estimate; Other = input - Vocals,
// so the stems sum to the input exactly (up to float rounding).
#include <algorithm>
#include <complex>
#include <cstddef>

#include "sonora/stems/separators.h"
#include "stft_util.h"

namespace sonora::stems {
using namespace detail;

bool CentreSeparator::supports(StemKind k) const {
    return channels_ >= 2 && (k == StemKind::Vocals || k == StemKind::Other);
}

bool CentreSeparator::separate(const StemRequest& r, StemResult& out, ProgressSink* p,
                               std::string* err) {
    auto fail = [&](const char* m) {
        if (err) *err = m;
        out.stems.clear();
        return false;
    };
    if (!valid_request(r)) return fail("bad request");
    if (r.channels < 2) return fail("centre separation needs stereo input");
    const std::int64_t F = stft_frames(r.frames);
    std::vector<double> acc(r.frames, 0.0);
    std::vector<std::complex<double>> l(kBins), rr(kBins), c(kBins);
    Stft stft;
    for (std::int64_t f = 0; f < F; ++f) {
        stft.analyze(r.in[0], r.frames, f, l.data());
        stft.analyze(r.in[1], r.frames, f, rr.data());
        for (int k = 0; k < kBins; ++k) {
            auto mid = 0.5 * (l[k] + rr[k]);
            double pm = std::norm(mid), ps = std::norm(0.5 * (l[k] - rr[k]));
            c[k] = (pm + ps > 1e-30) ? mid * (pm / (pm + ps)) : 0.0;
        }
        stft.synthesize(c.data(), r.frames, f, acc.data());
        if (p && !p->report(double(f + 1) / F)) return fail("cancelled");
    }
    const std::size_t n = std::size_t(r.channels) * r.frames;
    out.stems.assign(kStemKindCount, {});
    auto& voc = out.stems[static_cast<int>(StemKind::Vocals)];
    auto& oth = out.stems[static_cast<int>(StemKind::Other)];
    voc.resize(n);
    oth.resize(n);
    out.channels = r.channels;
    out.frames = r.frames;
    for (int ch = 0; ch < r.channels; ++ch)
        for (std::int64_t i = 0; i < r.frames; ++i) {
            float v = float(acc[i]);
            voc[ch * r.frames + i] = v;
            oth[ch * r.frames + i] = r.in[ch][i] - v;
        }
    return true;
}

}  // namespace sonora::stems
