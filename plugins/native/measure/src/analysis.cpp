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

std::vector<double> octave_band(const std::vector<float>& x, double fs, double band_hz, bool reversed) {
    // Sixth-order Butterworth band-pass between the octave's edges
    // (third-order high- and low-pass, matched designs), the class of
    // filter IEC 61260-1 describes. Above 0.49 fs the upper edge is left
    // open: the band is then everything past the lower edge.
    std::vector<double> y(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) y[i] = x[reversed ? x.size() - 1 - i : i];
    if (band_hz <= 0.0) return y;
    for (const bool high : {true, false}) {
        const double edge = high ? band_hz / std::sqrt(2.0) : band_hz * std::sqrt(2.0);
        if (!high && edge >= dsp::kMaxFreqRatio * fs) continue;
        dsp::FilterSpec secs[5];
        const int n = dsp::butterworth_sections(3, high, edge, secs);
        for (int k = 0; k < n; ++k) {
            const dsp::BiquadCoeffs c = dsp::design_matched(secs[k], fs);
            dsp::BiquadState st;
            for (double& v : y) v = st.tick(c, v);
        }
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
