// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/space_measure.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <vector>

#include "sonora/wav_reader.h"

namespace sonora {
namespace {

// Decay time: Schroeder's backward integration (J. Acoust. Soc. Am. 37,
// 1965) of the energy of free decays found in the programme, as blind
// estimators do (Loellmann et al., "An improved algorithm for blind
// reverberation time estimation", IWAENC 2010, also look for falling edges of
// the running level). Ours is the simpler, deterministic variant: segments
// that fall at least 30 dB are integrated and T20 / EDT fitted by least
// squares (ISO 3382-1); the median of the segments is reported. Pre-delay and
// direct-to-reverberant ratio come from isolated transients followed by a gap
// and then the tail (DRR as defined in Eaton et al., "Estimation of Room
// Acoustic Parameters: the ACE Challenge", IEEE/ACM TASLP 2016: direct-path
// energy over the energy after it).

double db(double power) { return 10.0 * std::log10(power + 1e-12); }

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 == 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double clampd(double v, double lo, double hi) { return std::min(hi, std::max(lo, v)); }

struct Fit {
    double slope = 0.0;   // dB per sample
    double r2 = 0.0;
};

Fit fit_line(const std::vector<double>& y, std::size_t i0, std::size_t i1) {
    Fit f;
    const double n = static_cast<double>(i1 - i0 + 1);
    double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0;
    for (std::size_t i = i0; i <= i1; ++i) {
        const double x = static_cast<double>(i);
        sx += x; sy += y[i]; sxx += x * x; sxy += x * y[i]; syy += y[i] * y[i];
    }
    const double dx = n * sxx - sx * sx;
    const double dy = n * syy - sy * sy;
    if (dx <= 0.0 || dy <= 0.0) return f;
    f.slope = (n * sxy - sx * sy) / dx;
    const double r = (n * sxy - sx * sy) / std::sqrt(dx * dy);
    f.r2 = r * r;
    return f;
}

// Prefix sums of x^2 give any window's mean power in O(1).
struct Power {
    std::vector<double> cum;
    explicit Power(const std::vector<float>& x) : cum(x.size() + 1, 0.0) {
        for (std::size_t i = 0; i < x.size(); ++i) cum[i + 1] = cum[i] + double(x[i]) * x[i];
    }
    std::size_t size() const { return cum.size() - 1; }
    double sum(std::size_t a, std::size_t b) const {
        a = std::min(a, size());
        b = std::min(b, size());
        return b > a ? cum[b] - cum[a] : 0.0;
    }
    double mean(std::size_t a, std::size_t b) const {
        b = std::min(b, size());
        return b > a ? sum(a, b) / double(b - a) : 0.0;
    }
};

bool local_max(const std::vector<double>& e, std::size_t k, std::size_t radius) {
    const std::size_t lo = k > radius ? k - radius : 0;
    const std::size_t hi = std::min(e.size(), k + radius + 1);
    for (std::size_t j = lo; j < k; ++j)
        if (e[j] >= e[k]) return false;
    for (std::size_t j = k + 1; j < hi; ++j)
        if (e[j] > e[k]) return false;
    return true;
}

void measure_decay(const std::vector<float>& x, const Power& pw, double fs, std::size_t h,
                   SpaceMeasure& out) {
    const std::size_t K = x.size() / h;
    if (K < 400) return;
    std::vector<double> e(K);   // 20 ms centred level, one value per ms
    for (std::size_t k = 0; k < K; ++k) {
        const std::size_t a = k >= 10 ? (k - 10) * h : 0;
        e[k] = db(pw.mean(a, (k + 10) * h));
    }
    const double gmax = *std::max_element(e.begin(), e.end());
    std::vector<double> t20s, edts;
    std::size_t resume = 0;
    for (std::size_t k = 0; k < K; ++k) {
        if (k < resume || e[k] < gmax - 40.0 || e[k] < -90.0 || !local_max(e, k, 150)) continue;
        std::size_t start = k;   // end of the plateau: where the note stops
        while (start + 1 < K && start - k < 1000 && e[start + 1] >= e[k] - 3.0) ++start;
        std::size_t end = start, argmin = start;
        double lo = e[start];
        for (std::size_t j = start; j < K && j - start < 6000; ++j) {
            if (e[j] < lo) { lo = e[j]; argmin = j; }
            if (e[j] > lo + 6.0) break;   // the next note
            end = j;
            if (e[j] < e[start] - 45.0) break;   // 20 dB past the T20 range: enough
        }
        resume = end + 1;
        if (e[argmin] > e[start] - 30.0) continue;   // never fell far enough
        const std::size_t stop = std::min(end, argmin);
        const std::size_t s0 = start * h, s1 = std::min(x.size(), (stop + 1) * h);
        std::vector<double> curve(s1 - s0);   // Schroeder curve
        double acc = 0.0;
        for (std::size_t i = s1 - s0; i-- > 0;) {
            acc += double(x[s0 + i]) * x[s0 + i];
            curve[i] = acc;
        }
        const double total = curve[0];
        if (total <= 0.0) continue;
        for (double& c : curve) c = 10.0 * std::log10(c / total + 1e-12);
        std::size_t i5 = 0, i10 = 0, i25 = 0;
        for (std::size_t i = 0; i < curve.size(); ++i) {
            if (i5 == 0 && curve[i] <= -5.0) i5 = i;
            if (i10 == 0 && curve[i] <= -10.0) i10 = i;
            if (i25 == 0 && curve[i] <= -25.0) { i25 = i; break; }
        }
        if (i5 == 0 || i10 == 0 || i25 <= i5 + 8) continue;
        const Fit t20 = fit_line(curve, i5, i25);
        if (t20.slope >= 0.0 || t20.r2 < 0.95) continue;
        t20s.push_back(-60.0 / (t20.slope * fs));
        const Fit edt = fit_line(curve, 0, i10);
        if (edt.slope < 0.0) edts.push_back(-60.0 / (edt.slope * fs));
    }
    out.decay_count = static_cast<int>(t20s.size());
    if (t20s.empty()) return;
    out.decay_found = true;
    out.rt60_s = median(t20s);
    out.edt_s = edts.empty() ? out.rt60_s : median(edts);
}

// Isolated transient, then a gap at least 10 dB down, then the tail rising
// 6 dB: the gap is the pre-delay, the energy split is the DRR.
void measure_transients(const std::vector<float>& x, const Power& pw, double fs, std::size_t h,
                        SpaceMeasure& out) {
    const std::size_t K = x.size() / h;
    if (K < 400) return;
    std::vector<double> d(K);   // 2 ms windows, 1 ms hop
    for (std::size_t k = 0; k < K; ++k) d[k] = db(pw.mean(k * h, (k + 2) * h));
    const double gmax = *std::max_element(d.begin(), d.end());
    std::vector<double> pre, drr;
    for (std::size_t k = 80; k + 160 < K; ++k) {
        if (d[k] < gmax - 40.0 || !local_max(d, k, 20)) continue;
        double before = -200.0;
        for (std::size_t j = k - 60; j + 3 <= k; ++j) before = std::max(before, d[j]);
        if (before > d[k] - 20.0) continue;
        double m = d[k + 1];
        std::size_t r = 0;
        for (std::size_t j = k + 1; j < k + 150; ++j) {
            m = std::min(m, d[j]);
            if (j > k + 1 && d[j] >= m + 6.0 && m <= d[k] - 10.0) { r = j; break; }
        }
        if (r == 0) continue;
        std::size_t last = r;
        while (last + 1 < K && last - k < 3000 && d[last + 1] > d[k] - 70.0 &&
               !(d[last + 1] > d[last] + 10.0 && last > r + 5))
            ++last;
        const double ed = pw.sum(k * h, r * h);
        const double er = pw.sum(r * h, (last + 1) * h);
        if (ed <= 0.0 || er <= 0.0) continue;
        pre.push_back(double(r - k) * 1000.0 * double(h) / fs);
        drr.push_back(10.0 * std::log10(ed / er));
    }
    if (pre.empty()) return;
    out.predelay_found = out.drr_found = true;
    out.predelay_ms = median(pre);
    out.drr_db = median(drr);
    out.reverb_wet_db = clampd(-out.drr_db, -40.0, 0.0);
}

struct Onsets {
    std::vector<double> amp;   // RMS of 5 ms blocks
    std::vector<double> o;     // positive amplitude step
    double block_s = 0.005;
};

Onsets onsets_of(const std::vector<float>& x, double fs) {
    Onsets s;
    const std::size_t b = std::max<std::size_t>(1, std::size_t(std::lround(fs * s.block_s)));
    s.block_s = double(b) / fs;
    const std::size_t J = x.size() / b;
    s.amp.resize(J);
    s.o.assign(J, 0.0);
    for (std::size_t j = 0; j < J; ++j) {
        double a = 0;
        for (std::size_t i = j * b; i < (j + 1) * b; ++i) a += double(x[i]) * x[i];
        s.amp[j] = std::sqrt(a / double(b));
        if (j > 0) s.o[j] = std::max(0.0, s.amp[j] - s.amp[j - 1]);
    }
    return s;
}

// Mean removed: a steady signal has a positive onset floor, and its
// autocorrelation would otherwise look like an echo at every lag.
std::vector<double> autocorr(std::vector<double> o, std::size_t max_lag) {
    double mean = 0.0;
    for (double v : o) mean += v;
    mean /= double(o.size());
    for (double& v : o) v -= mean;
    std::vector<double> r(max_lag + 1, 0.0);
    for (std::size_t l = 0; l <= max_lag; ++l)
        for (std::size_t j = 0; j + l < o.size(); ++j) r[l] += o[j] * o[j + l];
    return r;
}

double refine(const std::vector<double>& r, std::size_t l) {   // parabolic peak
    if (l == 0 || l + 1 >= r.size()) return double(l);
    const double a = r[l - 1], b = r[l], c = r[l + 1];
    const double den = a - 2.0 * b + c;
    return den == 0.0 ? double(l) : double(l) + 0.5 * (a - c) / den;
}

void measure_echo(const std::vector<float>& wet, double fs, SpaceMeasure& out) {
    const Onsets s = onsets_of(wet, fs);
    const std::size_t lmin = std::size_t(0.06 / s.block_s), lmax = std::size_t(1.5 / s.block_s);
    if (s.o.size() < 4 * lmax) return;
    const std::vector<double> r = autocorr(s.o, lmax);
    if (r[0] <= 0.0) return;
    double best = 0.0;
    for (std::size_t l = lmin; l < lmax; ++l)
        if (r[l] > r[l - 1] && r[l] >= r[l + 1]) best = std::max(best, r[l]);
    for (std::size_t l = lmin; l < lmax; ++l) {
        if (!(r[l] > r[l - 1] && r[l] >= r[l + 1]) || r[l] < 0.6 * best) continue;
        out.echo_strength = r[l] / r[0];
        if (out.echo_strength < 0.2) return;
        out.echo_found = true;
        out.echo_ms = refine(r, l) * s.block_s * 1000.0;
        // Level of the first repeat under its source, over clean onsets.
        const std::size_t lag = l;
        const double omax = *std::max_element(s.o.begin(), s.o.end());
        std::vector<double> ratios;
        for (std::size_t j = 14; j + lag + 3 < s.o.size(); ++j) {
            if (s.o[j] < 0.3 * omax || s.o[j] < s.o[j - 1] || s.o[j] < s.o[j + 1]) continue;
            const double src = std::max(s.amp[j], s.amp[j + 1]);
            double before = 0.0;
            for (std::size_t q = j - 12; q + 2 <= j; ++q) before = std::max(before, s.amp[q]);
            if (src <= 0.0 || before > 0.3 * src) continue;
            double rep = 0.0;
            for (std::size_t q = j + lag - 2; q <= j + lag + 2; ++q) rep = std::max(rep, s.amp[q]);
            const double ratio = 20.0 * std::log10(rep / src + 1e-12);
            if (ratio > -40.0) ratios.push_back(ratio);
        }
        if (!ratios.empty()) {
            out.echo_level_found = true;
            out.delay_wet_db = clampd(median(ratios), -40.0, 0.0);
        }
        return;
    }
}

void measure_tempo(const std::vector<float>& mono, double fs, SpaceMeasure& out) {
    const Onsets s = onsets_of(mono, fs);
    const std::size_t lmin = std::size_t(0.375 / s.block_s), lmax = std::size_t(0.75 / s.block_s);
    if (s.o.size() < 8 * lmax) return;
    const std::vector<double> r = autocorr(s.o, lmax + 1);
    if (r[0] <= 0.0) return;
    std::size_t arg = 0;
    for (std::size_t l = lmin; l <= lmax; ++l)
        if (r[l] > r[l - 1] && r[l] >= r[l + 1] && (arg == 0 || r[l] > r[arg])) arg = l;
    if (arg == 0 || r[arg] / r[0] < 0.1) return;
    out.tempo_found = true;
    out.tempo_bpm = 60.0 / (refine(r, arg) * s.block_s);
}

}  // namespace

bool measure_space(const float* in, std::size_t frames, unsigned channels, double fs,
                   SpaceMeasure& out, std::string& error) {
    out = SpaceMeasure{};
    if (in == nullptr || channels == 0 || fs < 8000.0 || fs > 384000.0) {
        error = "the reference audio has no usable format";
        return false;
    }
    if (double(frames) < fs) {
        error = "the reference is shorter than 1 s";
        return false;
    }
    out.sample_rate = fs;
    out.duration_s = double(frames) / fs;
    std::vector<float> mid(frames), side;
    double em = 0, es = 0;
    if (channels >= 2) side.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const float l = in[i * channels], r = channels >= 2 ? in[i * channels + 1] : l;
        mid[i] = 0.5f * (l + r);
        em += double(mid[i]) * mid[i];
        if (!side.empty()) {
            side[i] = 0.5f * (l - r);
            es += double(side[i]) * side[i];
        }
    }
    if (em <= 1e-12 * double(frames)) {
        error = "the reference is silent";
        return false;
    }
    const std::size_t h = std::max<std::size_t>(1, std::size_t(std::lround(fs / 1000.0)));
    const Power pw(mid);
    measure_decay(mid, pw, fs, h, out);
    measure_transients(mid, pw, fs, h, out);
    // A stereo echo that ping-pongs lives in the side signal; otherwise the mid.
    measure_echo(!side.empty() && es > 0.1 * em ? side : mid, fs, out);
    measure_tempo(mid, fs, out);
    return true;
}

bool measure_space_file(const std::string& path, SpaceMeasure& out, std::string& error) {
    const std::size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == "flac") {
        error = path + ": FLAC is not read yet; convert it to WAV (flac -d) first";
        return false;
    }
    WavReader rd;
    std::string why;
    if (!rd.open(path, why)) {
        error = path + ": " + why;
        return false;
    }
    const int64_t cap = int64_t(rd.sample_rate()) * 900;
    const int64_t n = std::min<int64_t>(rd.frames(), cap);
    const unsigned ch = std::min<unsigned>(rd.channels(), 2);
    std::vector<float> data(static_cast<std::size_t>(n) * ch), raw;
    constexpr int64_t kBlock = 1 << 16;
    for (int64_t at = 0; at < n; at += kBlock) {
        const int64_t want = std::min(kBlock, n - at);
        raw.resize(static_cast<std::size_t>(want) * rd.channels());
        const int64_t got = rd.read(at, want, raw.data());
        if (got != want) {
            error = path + ": read error";
            return false;
        }
        for (int64_t i = 0; i < want; ++i)
            for (unsigned c = 0; c < ch; ++c)
                data[static_cast<std::size_t>(at + i) * ch + c] =
                    raw[static_cast<std::size_t>(i) * rd.channels() + c];
    }
    if (!measure_space(data.data(), static_cast<std::size_t>(n), ch, rd.sample_rate(), out, why)) {
        error = path + ": " + why;
        return false;
    }
    return true;
}

ReverbDelayReference to_reference(const SpaceMeasure& m, ReverbDelayReference base) {
    if (m.decay_found) base.decay_s = clampd(m.rt60_s, 0.2, 8.0);
    if (m.predelay_found) base.predelay_ms = clampd(m.predelay_ms, 0.0, 200.0);
    if (m.drr_found) base.reverb_wet_db = m.reverb_wet_db;
    if (m.echo_found) base.delay_ms = m.echo_ms;
    if (m.echo_level_found) base.delay_wet_db = m.delay_wet_db;
    return base;
}

std::string describe_space(const SpaceMeasure& m) {
    char b[160];
    std::string s;
    if (m.decay_found)
        std::snprintf(b, sizeof b, "decay: RT60 %.2f s (T20), EDT %.2f s, from %d decays\n", m.rt60_s,
                      m.edt_s, m.decay_count);
    else
        std::snprintf(b, sizeof b, "decay: no free decay of 30 dB found; the default stays\n");
    s += b;
    if (m.predelay_found)
        std::snprintf(b, sizeof b, "pre-delay: %.1f ms, direct-to-reverberant %.1f dB\n",
                      m.predelay_ms, m.drr_db);
    else
        std::snprintf(b, sizeof b,
                      "pre-delay: no isolated transient with a gap; the defaults stay\n");
    s += b;
    if (m.echo_found)
        std::snprintf(b, sizeof b, "echo: %.0f ms (strength %.2f)\n", m.echo_ms, m.echo_strength);
    else
        std::snprintf(b, sizeof b, "echo: no repeating echo found; the default stays\n");
    s += b;
    if (m.echo_level_found)
        std::snprintf(b, sizeof b, "echo level: %.1f dB under its source\n", m.delay_wet_db);
    else
        std::snprintf(b, sizeof b, "echo level: no clean repeat to compare; the default stays\n");
    s += b;
    if (m.tempo_found)
        std::snprintf(b, sizeof b, "tempo: %.1f bpm\n", m.tempo_bpm);
    else
        std::snprintf(b, sizeof b, "tempo: no steady pulse found\n");
    s += b;
    return s;
}

}  // namespace sonora
