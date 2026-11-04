// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>

#include "sonora/dsp/fft.h"
#include "sonora/native/vocal_balance_analysis.h"

namespace sonora::native {

namespace {

// The processor's band count (vocal_balance.h); this library does not link the SDK.
constexpr std::size_t kMaxBands = 5;
constexpr double kPi = 3.14159265358979323846;
constexpr double kLowHz = 100.0;
constexpr double kHighHz = 16000.0;
constexpr double kResonanceFloorHz = 250.0;  // below it the window is under 6 bins and harmonics dominate
constexpr double kResonanceCeilHz = 12000.0;  // above it a voice has little, and band limits of the recording show
constexpr double kMinExcessDb = 3.0;
constexpr double kPercentile = 0.30;
constexpr int kHistBins = 140;               // 0.5 dB from -30 to +40
constexpr double kHistFloor = -30.0;
constexpr double kSibLoHz = 4500.0;
constexpr double kSibHiHz = 11000.0;
constexpr double kZoneActDb = 4.0;

double db10(double x) { return 10.0 * std::log10(x + 1e-20); }

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    const std::size_t k = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

struct ZoneDef {
    const char* name;
    double lo, hi;
};
const ZoneDef kZones[] = {
    {"proximity", 100.0, 250.0}, {"boxiness", 250.0, 500.0}, {"nasal", 700.0, 1400.0},
    {"presence", 2500.0, 5000.0}, {"sibilance", 5000.0, 10000.0}, {"air", 10000.0, 16000.0},
};

}  // namespace

bool analyse_vocal(const float* mono, std::size_t frames, double fs, VocalAnalysis& out,
                   std::string& error) {
    out = VocalAnalysis{};
    error.clear();
    if (!mono || !(fs >= 8000.0)) {
        error = "no audio, or a sample rate under 8 kHz";
        return false;
    }
    const std::uint32_t n = fs < 70000.0 ? 4096u : 8192u;
    const std::uint32_t hop = n / 2;
    if (frames < 2u * n) {
        error = "the audio is too short to analyse: " + std::to_string(frames) + " samples, at least " +
                std::to_string(2u * n) + " needed";
        return false;
    }
    for (std::size_t i = 0; i < frames; ++i) {
        if (!std::isfinite(mono[i])) {
            error = "the audio holds a non-finite sample";
            return false;
        }
    }
    out.sample_rate = fs;
    out.duration_s = static_cast<double>(frames) / fs;

    const std::size_t nframes = (frames - n) / hop + 1;
    std::vector<double> frame_db(nframes);
    for (std::size_t f = 0; f < nframes; ++f) {
        double s = 0.0;
        const float* p = mono + f * hop;
        for (std::uint32_t i = 0; i < n; ++i) s += static_cast<double>(p[i]) * p[i];
        frame_db[f] = db10(s / n);
    }
    const double ref = percentile(frame_db, 0.95);
    if (ref < -70.0) {
        error = "too quiet to hold a voice: the loudest frames are at " + std::to_string(ref) + " dBFS rms";
        return false;
    }
    const double gate = std::max(ref - 30.0, -70.0);
    std::vector<std::size_t> active;
    std::vector<double> active_db;
    for (std::size_t f = 0; f < nframes; ++f) {
        if (frame_db[f] >= gate) {
            active.push_back(f);
            active_db.push_back(frame_db[f]);
        }
    }
    out.active_fraction = static_cast<double>(active.size()) / static_cast<double>(nframes);
    out.level_db = percentile(active_db, 0.5);
    out.level_spread_db = percentile(active_db, 0.9) - percentile(active_db, 0.1);

    // Spectra.
    dsp::RealFft fft;
    fft.prepare(n);
    std::vector<double> win(n), buf(n);
    double sumw2 = 0.0;
    for (std::uint32_t i = 0; i < n; ++i) {
        win[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / n);
        sumw2 += win[i] * win[i];
    }
    const double bin_hz = fs / n;
    const std::uint32_t kmin = static_cast<std::uint32_t>(std::ceil(kLowHz / bin_hz));
    const std::uint32_t kmax = static_cast<std::uint32_t>(std::floor(std::min(kHighHz, 0.45 * fs) / bin_hz));
    const std::uint32_t nb = kmax - kmin + 1;
    std::vector<std::complex<double>> spec(n / 2 + 1);
    std::vector<double> ms(n / 2 + 1), dbk(n / 2 + 1), pre(n / 2 + 2);
    std::vector<std::uint32_t> hist(static_cast<std::size_t>(nb) * kHistBins, 0);
    std::vector<double> mean_db(nb, 0.0);
    struct FrameHf {
        double total_ms, hf_ms, fmoment;
    };
    std::vector<FrameHf> hf;
    hf.reserve(active.size());
    const std::uint32_t hf_lo = static_cast<std::uint32_t>(std::ceil(kSibLoHz / bin_hz));
    const std::uint32_t hf_hi = static_cast<std::uint32_t>(std::floor(std::min(kSibHiHz, 0.45 * fs) / bin_hz));

    for (const std::size_t f : active) {
        const float* p = mono + f * hop;
        for (std::uint32_t i = 0; i < n; ++i) buf[i] = static_cast<double>(p[i]) * win[i];
        fft.forward(buf.data(), spec.data());
        FrameHf h{0.0, 0.0, 0.0};
        for (std::uint32_t k = kmin; k <= kmax; ++k) {
            ms[k] = 2.0 * std::norm(spec[k]) / (static_cast<double>(n) * sumw2);
            dbk[k] = db10(ms[k]);
            h.total_ms += ms[k];
            if (k >= hf_lo && k <= hf_hi) {
                h.hf_ms += ms[k];
                h.fmoment += ms[k] * k * bin_hz;
            }
        }
        hf.push_back(h);
        // Prefix sums of the dB values, for the mean over +-1/6 octave.
        pre[kmin] = 0.0;
        for (std::uint32_t k = kmin; k <= kmax; ++k) pre[k + 1] = pre[k] + dbk[k];
        for (std::uint32_t k = kmin; k <= kmax; ++k) {
            std::uint32_t lo = static_cast<std::uint32_t>(std::floor(k * 0.8909));
            std::uint32_t hi = static_cast<std::uint32_t>(std::ceil(k * 1.1225));
            lo = std::max(lo, kmin);
            hi = std::min(hi, kmax);
            const double mean = (pre[hi + 1] - pre[lo]) / static_cast<double>(hi - lo + 1);
            const double ex = dbk[k] - mean;
            int b = static_cast<int>(std::floor((ex - kHistFloor) * 2.0));
            b = std::max(0, std::min(kHistBins - 1, b));
            ++hist[static_cast<std::size_t>(k - kmin) * kHistBins + static_cast<std::size_t>(b)];
            mean_db[k - kmin] += dbk[k];
        }
    }
    const double na = static_cast<double>(active.size());

    // Persistent excess per bin.
    std::vector<double> q30(nb, -30.0);
    for (std::uint32_t j = 0; j < nb; ++j) {
        const double want = kPercentile * na;
        double cum = 0.0;
        for (int b = 0; b < kHistBins; ++b) {
            cum += hist[static_cast<std::size_t>(j) * kHistBins + static_cast<std::size_t>(b)];
            if (cum >= want) {
                q30[j] = kHistFloor + 0.5 * b + 0.25;
                break;
            }
        }
        mean_db[j] /= na;
    }

    // Peaks of the persistent excess.
    struct Cand {
        double f, q, ex;
    };
    std::vector<Cand> cands;
    const std::uint32_t pk_hi = std::min<std::uint32_t>(kmax, static_cast<std::uint32_t>(kResonanceCeilHz / bin_hz));
    const std::uint32_t pk_lo = std::max<std::uint32_t>(kmin + 1, static_cast<std::uint32_t>(std::ceil(kResonanceFloorHz / bin_hz)));
    for (std::uint32_t k = pk_lo; k + 1 <= pk_hi; ++k) {
        const double a = q30[k - 1 - kmin], b = q30[k - kmin], c = q30[k + 1 - kmin];
        if (b < kMinExcessDb || b <= a || b < c) continue;
        double delta = 0.0;
        const double den = a - 2.0 * b + c;
        if (den < 0.0) delta = 0.5 * (a - c) / den;
        const double f = (k + delta) * bin_hz;
        // Width at half the excess, walking out from the peak.
        std::uint32_t l = k, r = k;
        while (l > kmin && q30[l - kmin] > 0.5 * b) --l;
        while (r < kmax && q30[r - kmin] > 0.5 * b) ++r;
        // A resonance falls away on both sides at a similar rate. The edge
        // of a band limit (a codec's low-pass, an anti-alias filter) leaves
        // a plateau on one side and a drop on the other: not a resonance.
        const double up = std::max(1.0, static_cast<double>(r - k)), down = std::max(1.0, static_cast<double>(k - l));
        if (up > 4.0 * down || down > 4.0 * up) continue;
        const double width = std::max(static_cast<double>(r - l), 1.0) * bin_hz;
        cands.push_back({f, std::min(16.0, std::max(1.0, f / width)), b});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.ex > y.ex; });
    std::vector<Cand> kept;
    for (const Cand& c : cands) {
        bool crowded = false;
        for (const Cand& k : kept) {
            const double r = c.f > k.f ? c.f / k.f : k.f / c.f;
            if (r < 1.189207) crowded = true;  // 2^(1/4)
        }
        if (crowded) continue;
        kept.push_back(c);
        if (kept.size() == kMaxBands) break;
    }
    std::sort(kept.begin(), kept.end(), [](const Cand& x, const Cand& y) { return x.f < y.f; });
    for (const Cand& c : kept) {
        VocalResonance r;
        r.freq_hz = c.f;
        r.q = c.q;
        r.excess_db = c.ex;
        r.cut_db = std::min(12.0, std::max(2.0, 0.7 * c.ex));
        r.bell_q = std::min(12.0, std::max(1.5, 0.8 * c.q));
        for (const ZoneDef& z : kZones) {
            if (c.f >= z.lo && c.f < z.hi) r.zone = z.name;
        }
        out.resonances.push_back(r);
    }

    // Zones against the line through the mean spectrum.
    {
        double sx = 0, sy = 0, sxx = 0, sxy = 0, cnt = 0;
        const double top = std::min(12000.0, 0.45 * fs);
        for (std::uint32_t k = kmin; k <= kmax; ++k) {
            const double f = k * bin_hz;
            if (f < 120.0 || f > top) continue;
            const double x = std::log2(f), y = mean_db[k - kmin];
            sx += x;
            sy += y;
            sxx += x * x;
            sxy += x * y;
            cnt += 1;
        }
        const double den = cnt * sxx - sx * sx;
        const double slope = den > 0 ? (cnt * sxy - sx * sy) / den : 0.0;
        const double icpt = cnt > 0 ? (sy - slope * sx) / cnt : 0.0;
        for (const ZoneDef& z : kZones) {
            double s = 0;
            int c = 0;
            for (std::uint32_t k = kmin; k <= kmax; ++k) {
                const double f = k * bin_hz;
                if (f < z.lo || f >= z.hi) continue;
                // A resonance already found is not also a tilt: leave its
                // 1/6 octave out, or it would be cut twice.
                bool masked = false;
                for (const Cand& r : kept) {
                    if (f > r.f / 1.1225 && f < r.f * 1.1225) masked = true;
                }
                if (masked) continue;
                s += mean_db[k - kmin] - (icpt + slope * std::log2(f));
                ++c;
            }
            if (c == 0) continue;
            out.zones.push_back({z.name, z.lo, z.hi, s / c});
        }
    }

    // Sibilance.
    {
        std::vector<double> sib_lvl, other_lvl;
        double mom = 0.0, pw = 0.0;
        for (const FrameHf& h : hf) {
            if (h.total_ms > 0.0 && h.hf_ms / h.total_ms > 0.5) {
                sib_lvl.push_back(db10(2.0 * h.hf_ms));
                mom += h.fmoment;
                pw += h.hf_ms;
            } else {
                other_lvl.push_back(db10(2.0 * h.hf_ms));
            }
        }
        out.sibilance.fraction = static_cast<double>(sib_lvl.size()) / na;
        if (sib_lvl.size() >= 3 && out.sibilance.fraction >= 0.002 && pw > 0.0) {
            out.sibilance.found = true;
            out.sibilance.freq_hz = mom / pw;
            out.sibilance.level_db = percentile(sib_lvl, 0.5);
            out.sibilance.excess_db =
                other_lvl.empty() ? 0.0 : out.sibilance.level_db - percentile(other_lvl, 0.5);
        }
    }
    return true;
}

std::vector<std::pair<std::string, float>> vocal_balance_params(const VocalAnalysis& a) {
    static const float kDefaults[kMaxBands] = {280.0f, 1000.0f, 3300.0f, 4600.0f, 7500.0f};
    std::vector<std::pair<std::string, float>> p;
    const std::size_t found = std::min<std::size_t>(a.resonances.size(), kMaxBands);
    const std::size_t n = std::max<std::size_t>(3, found);
    p.emplace_back("bands", static_cast<float>(n - 3));
    std::vector<double> qs;
    for (std::size_t i = 0; i < kMaxBands; ++i) {
        const std::string idx = std::to_string(i + 1);
        if (i < found) {
            p.emplace_back("freq" + idx, static_cast<float>(std::min(16000.0, std::max(100.0, a.resonances[i].freq_hz))));
            p.emplace_back("cut" + idx, static_cast<float>(a.resonances[i].cut_db));
            qs.push_back(a.resonances[i].bell_q);
        } else {
            // Padding: a band with no cap does nothing, wherever it sits.
            p.emplace_back("freq" + idx, kDefaults[i]);
            p.emplace_back("cut" + idx, 0.0f);
        }
    }
    p.emplace_back("sharpness", static_cast<float>(qs.empty() ? 4.0 : percentile(qs, 0.5)));
    // The analysis already chose persistent peaks, so the live detector only
    // has to follow them: a low threshold and full depth, the cap per band
    // being the analysis's cut. (Measured: against a voice the wide detector
    // also holds the skirts of the loud low harmonics, so a resonance reads
    // about 6 dB less excess than against noise; the default threshold of 6 dB
    // left a 22 dB-over resonance cut by 6 of its 12 dB.)
    p.emplace_back("threshold", 2.0f);
    p.emplace_back("depth", 100.0f);
    return p;
}

}  // namespace sonora::native
