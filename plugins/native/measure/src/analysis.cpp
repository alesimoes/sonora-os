// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>

#include "sonora/dsp/biquad.h"
#include "sonora/dsp/fft.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/reference_peak.h"
#include "sonora/dsp/true_peak.h"
#include "sonora/measure/measure.h"

namespace sonora::measure {

using dsp::kPi;
using dsp::kTwoPi;

double peak(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double m = 0.0;
    to = std::min(to, x.size());
    for (std::size_t i = from; i < to; ++i) m = std::fmax(m, std::fabs(x[i]));
    return m;
}

double rms(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double e = 0.0;
    to = std::min(to, x.size());
    if (to <= from) return 0.0;
    for (std::size_t i = from; i < to; ++i) e += static_cast<double>(x[i]) * x[i];
    return std::sqrt(e / static_cast<double>(to - from));
}

double to_db(double v) { return v > 0.0 ? 20.0 * std::log10(v) : -400.0; }

std::pair<double, double> response_db_deg(const std::vector<float>& ir, double f, double fs,
                                          double delay) {
    // The whole response, pre-ringing of a linear-phase stage included,
    // with the reported delay taken out as a pure phase term.
    std::complex<double> acc(0.0, 0.0);
    const double w = kTwoPi * f / fs;
    for (std::size_t n = 0; n < ir.size(); ++n) {
        acc += static_cast<double>(ir[n]) * std::polar(1.0, -w * (static_cast<double>(n) - delay));
    }
    return {to_db(std::abs(acc)), std::arg(acc) * 180.0 / kPi};
}

namespace {

constexpr std::uint32_t kN = 32768;

// Power spectrum of y[from, from + kN), Kaiser window with beta 20: its
// sidelobes are far under -150 dB, so the window's own leakage stays below
// a -110 dB THD+N bar (a 4-term Blackman-Harris floors at about -90 dB).
std::vector<double> spectrum(const std::vector<float>& y, std::size_t from) {
    dsp::Fft fft;
    fft.prepare(kN);
    std::vector<std::complex<double>> x(kN);
    for (std::uint32_t i = 0; i < kN; ++i) {
        const double w = dsp::kaiser((2.0 * i) / (kN - 1) - 1.0, 20.0);
        const std::size_t k = from + i;
        x[i] = (k < y.size() ? y[k] : 0.0f) * w;
    }
    fft.forward(x.data());
    std::vector<double> p(kN / 2);
    for (std::uint32_t k = 0; k < kN / 2; ++k) p[k] = std::norm(x[k]);
    return p;
}

// Bins within the window's main lobe of frequency f (beta 20: about 6.5
// bins each side, and its skirt).
bool near_bin(std::uint32_t k, double f, double fs) {
    return std::fabs(k - f * kN / fs) <= 12.0;
}

}  // namespace

double thdn_db(const std::vector<float>& y, std::size_t from, double fs, double f0) {
    const auto p = spectrum(y, from);
    double fund = 0.0, rest = 0.0;
    for (std::uint32_t k = 2; k < p.size(); ++k) {
        // 20 Hz to 20 kHz, as an analyser's THD+N bandwidth.
        const double f = k * fs / kN;
        if (f < 20.0 || f > 20000.0) continue;
        if (near_bin(k, f0, fs)) fund += p[k];
        else rest += p[k];
    }
    return 10.0 * std::log10(rest / fund);
}

double alias_dbc(const std::vector<float>& y, std::size_t from, double fs, double f0) {
    const auto p = spectrum(y, from);
    double fund = 0.0, other = 0.0;
    for (std::uint32_t k = 2; k < p.size(); ++k) {
        const double f = k * fs / kN;
        const double h = std::round(f / f0);
        const bool harmonic = h >= 1.0 && near_bin(k, h * f0, fs);
        if (harmonic && h == 1.0) fund += p[k];
        else if (!harmonic && f >= 20.0 && f <= 20000.0) other += p[k];
    }
    return 10.0 * std::log10(other / fund);
}

long xcorr_lag(const std::vector<float>& in, const std::vector<float>& out, long max_lag) {
    long best = 0;
    double best_v = -1.0;
    const long n = static_cast<long>(std::min(in.size(), out.size()));
    for (long lag = 0; lag <= max_lag; ++lag) {
        double acc = 0.0;
        for (long i = 0; i + lag < n; ++i) acc += static_cast<double>(in[i]) * out[i + lag];
        if (std::fabs(acc) > best_v) {
            best_v = std::fabs(acc);
            best = lag;
        }
    }
    return best;
}

double hf_residue_peak(const std::vector<float>& y, double fs, std::size_t from) {
    dsp::FilterSpec secs[5];
    const int n = dsp::butterworth_sections(8, true, 5000.0, secs);
    dsp::BiquadCoeffs c[5];
    dsp::BiquadState s[5];
    for (int k = 0; k < n; ++k) c[k] = dsp::design_matched(secs[k], fs);
    double m = 0.0;
    for (std::size_t i = 0; i < y.size(); ++i) {
        double v = y[i];
        for (int k = 0; k < n; ++k) v = s[k].tick(c[k], v);
        if (i >= from) m = std::fmax(m, std::fabs(v));
    }
    return m;
}

int octave_band_sections(double fs, double band_hz, dsp::BiquadCoeffs* out) {
    // A third-order Butterworth low-pass prototype taken to a band-pass by
    // s -> (s^2 + w0^2) / (B s), so sixth order with its -3 dB points on the
    // octave's edges fc 2^(-1/2) and fc 2^(1/2) (base-2 octaves, as ISO
    // 3382-1 and IEC 61260-1 define them), then the bilinear transform with
    // both edges prewarped, so they stay exact at every rate. In analog
    // terms its magnitude is 1 / sqrt(1 + W^6), W = sqrt(2) (f/fc - fc/f):
    // -3.0 dB at the edges, -19.6 dB one octave from the centre, -43.4 dB
    // two octaves out and -62.6 dB three. The digital filter keeps the
    // edges and the centre exactly and the skirts within 0.5 dB of these
    // for fc <= fs / 10; nearer Nyquist the prewarped axis compresses the
    // lower skirt (the 8 kHz band at 44.1 kHz: 18.0 and 40.9 dB one and two
    // octaves below). plugins/native/reverb's meter_check measures it.
    // Above 0.49 fs the upper edge is left open: the band is then
    // everything past the lower edge, a third-order Butterworth high-pass.
    const double lo = band_hz / std::sqrt(2.0);
    const double hi = band_hz * std::sqrt(2.0);
    if (hi >= dsp::kMaxFreqRatio * fs) {
        dsp::FilterSpec secs[5];
        const int n = dsp::butterworth_sections(3, true, lo, secs);
        for (int k = 0; k < n; ++k) out[k] = dsp::design_matched(secs[k], fs);
        return n;
    }
    const double c = 2.0 * fs;
    const double w1 = c * std::tan(kPi * lo / fs);
    const double w2 = c * std::tan(kPi * hi / fs);
    const double w0sq = w1 * w2;
    const double bw = w2 - w1;
    // The prototype's real pole, -1, gives the analog section
    // B s / (s^2 + B s + w0^2), taken through the bilinear transform as it
    // stands (its poles are real when the band is wide in the prewarped
    // axis, as the top octaves are near Nyquist). Its complex pair
    // -1/2 +- j sqrt(3)/2 gives the roots of s^2 - p B s + w0^2, never real
    // for a complex p, each one section with its conjugate. Every section
    // has a zero at s = 0 and one at infinity (z = 1 and z = -1).
    {
        const double a0 = c * c + bw * c + w0sq;
        dsp::BiquadCoeffs& q = out[0];
        q.b0 = bw * c / a0;
        q.b1 = 0.0;
        q.b2 = -q.b0;
        q.a1 = 2.0 * (w0sq - c * c) / a0;
        q.a2 = (c * c - bw * c + w0sq) / a0;
    }
    const std::complex<double> pb = std::complex<double>(-0.5, std::sqrt(3.0) / 2.0) * bw;
    const std::complex<double> root = std::sqrt(pb * pb - 4.0 * w0sq);
    for (int k = 1; k <= 2; ++k) {
        const std::complex<double> sp = 0.5 * (pb + (k == 1 ? 1.0 : -1.0) * root);
        const std::complex<double> z = (c + sp) / (c - sp);
        dsp::BiquadCoeffs& q = out[k];
        q.b0 = 1.0;
        q.b1 = 0.0;
        q.b2 = -1.0;
        q.a1 = -2.0 * z.real();
        q.a2 = std::norm(z);
    }
    // Unity at the digital image of w0, the analog prototype's peak.
    const double w0d = 2.0 * std::atan(std::sqrt(w0sq) / c);
    const std::complex<double> zi = std::polar(1.0, -w0d);  // z^-1 there
    std::complex<double> h = 1.0;
    for (int k = 0; k < 3; ++k) {
        const dsp::BiquadCoeffs& q = out[k];
        h *= (q.b0 + q.b1 * zi + q.b2 * zi * zi) / (1.0 + q.a1 * zi + q.a2 * zi * zi);
    }
    const double g = 1.0 / std::abs(h);
    out[1].b0 *= g;
    out[1].b2 *= g;
    return 3;
}

std::vector<double> octave_band(const std::vector<float>& x, double fs, double band_hz, bool reversed) {
    std::vector<double> y(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) y[i] = x[reversed ? x.size() - 1 - i : i];
    if (band_hz <= 0.0) return y;
    dsp::BiquadCoeffs secs[5];
    const int n = octave_band_sections(fs, band_hz, secs);
    for (int k = 0; k < n; ++k) {
        dsp::BiquadState st;
        for (double& v : y) v = st.tick(secs[k], v);
    }
    return y;
}

double t30_seconds(const Chans& irs, double fs, double band_hz, std::vector<double>* edc_db) {
    if (irs.empty() || irs[0].empty()) return 0.0;
    // Time-reversed filtering (ISO 3382-1:2009 §5.3.3, after Jacobsen and
    // Rindel): the band filter's own ringing then precedes the decay
    // instead of lengthening it, which matters for short T60 in low bands.
    // Schroeder backward integration of the forward-time response is a
    // forward running sum of the reversed one; the responses' energies are
    // summed first, as ISO 3382 averages decay curves over source and
    // receiver positions.
    const std::size_t len = irs[0].size();
    std::vector<double> sq(len, 0.0);
    for (const auto& ir : irs) {
        const std::vector<double> rev = octave_band(ir, fs, band_hz, true);
        for (std::size_t i = 0; i < len && i < rev.size(); ++i) sq[i] += rev[i] * rev[i];
    }
    std::vector<double> e(len);
    double acc = 0.0;
    for (std::size_t i = 0; i < len; ++i) {
        acc += sq[i];
        e[len - 1 - i] = acc;
    }
    if (!(acc > 0.0)) return 0.0;
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, n = 0.0;
    bool reached = false;
    for (std::size_t i = 0; i < e.size(); ++i) {
        const double db = 10.0 * std::log10(e[i] / acc + 1e-300);
        if (edc_db != nullptr) edc_db->push_back(db);
        if (db < -35.0) {
            reached = true;
            continue;
        }
        if (db > -5.0) continue;
        const double t = static_cast<double>(i) / fs;
        sx += t;
        sy += db;
        sxx += t * t;
        sxy += t * db;
        n += 1.0;
    }
    if (!reached || n < 2.0) return 0.0;
    const double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    return slope < 0.0 ? -60.0 / slope : 0.0;
}

double t30_seconds(const std::vector<float>& ir, double fs, double band_hz, std::vector<double>* edc_db) {
    return t30_seconds(Chans{ir}, fs, band_hz, edc_db);
}

double reference_true_peak(const std::vector<float>& y, double near_level) {
    return dsp::reference_true_peak(y.data(), y.size(), near_level);
}

double meter_true_peak(const std::vector<float>& y) {
    dsp::TruePeakDetector tp;
    tp.prepare(1);
    double m = 0.0;
    for (float v : y) m = std::fmax(m, tp.process(0, v));
    // Flush the interpolator, so peaks in the last samples are read too.
    for (std::uint32_t i = 0; i < 2 * tp.delay(); ++i) m = std::fmax(m, tp.process(0, 0.0));
    return m;
}

bool quick() {
    const char* q = std::getenv("SONORA_DSP_QUICK");
    return q != nullptr && std::strcmp(q, "1") == 0;
}

}  // namespace sonora::measure
