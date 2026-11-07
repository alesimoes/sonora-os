// SPDX-License-Identifier: GPL-3.0-or-later
//
// Welch long-term third-octave spectrum (ADR-0066 §4). P. D. Welch, "The use
// of FFT for the estimation of power spectra", IEEE Trans. Audio
// Electroacoust. 15(2), 1967: windowed, overlapped frames, power-averaged.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>

#include "sonora/dsp/fft.h"
#include "timbre_fit.h"

namespace sonora::analyze {
namespace {

constexpr std::uint32_t kWindow = 8192, kHop = 4096;
constexpr double kAbsoluteGateDb = -70.0;  // frame mean square, dBFS
constexpr double kRelativeGateDb = -20.0;  // under the mean of the frames above the absolute gate
constexpr double kPi = 3.14159265358979323846;

struct Frame {
    double power = 0.0;                       // mean square, summed over the channels measured
    std::array<double, kTimbreBands> band{};  // mean power per bin in each band
};

class FrameAnalyser {
public:
    explicit FrameAnalyser(double rate) : rate_(rate), window_(kWindow), x_(kWindow), spec_(kWindow / 2 + 1) {
        fft_.prepare(kWindow);
        for (std::uint32_t i = 0; i < kWindow; ++i) window_[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / kWindow);
        const double edge = std::pow(2.0, 1.0 / 6.0);  // third-octave band edges
        const double bin_hz = rate / kWindow;
        for (int b = 0; b < kTimbreBands; ++b) {
            lo_[b] = static_cast<std::uint32_t>(std::ceil(kThirdOctaveCentreHz[b] / edge / bin_hz));
            hi_[b] = static_cast<std::uint32_t>(std::ceil(kThirdOctaveCentreHz[b] * edge / bin_hz));
        }
    }

    void analyse(const std::vector<std::vector<double>>& channels, Frame& f) {
        f = Frame{};
        for (const auto& ch : channels) {
            double ms = 0.0;
            for (std::uint32_t i = 0; i < kWindow; ++i) {
                ms += ch[i] * ch[i];
                x_[i] = ch[i] * window_[i];
            }
            f.power += ms / kWindow;
            fft_.forward(x_.data(), spec_.data());
            for (int b = 0; b < kTimbreBands; ++b) {
                if (!usable(b)) continue;
                double s = 0.0;
                for (std::uint32_t k = lo_[b]; k < hi_[b]; ++k) s += std::norm(spec_[k]);
                f.band[b] += s / (hi_[b] - lo_[b]);
            }
        }
    }

    // Upper edge under Nyquist and at least one bin inside.
    bool usable(int b) const {
        return kThirdOctaveCentreHz[b] * std::pow(2.0, 1.0 / 6.0) < rate_ / 2.0 && hi_[b] > lo_[b] &&
               hi_[b] <= kWindow / 2 + 1;
    }

private:
    double rate_;
    sonora::dsp::RealFft fft_;
    std::vector<double> window_, x_;
    std::vector<std::complex<double>> spec_;
    std::uint32_t lo_[kTimbreBands]{}, hi_[kTimbreBands]{};
};

}  // namespace

bool WelchTimbreMeasurer::measure(AudioSource& source, const RunOptions& run, TimbreCurve& out,
                                  std::string& error) {
    const std::uint32_t nch = std::min<std::uint32_t>(source.channels(), 2);
    const double rate = source.sample_rate();
    const std::int64_t total = source.frames();
    if (nch == 0 || rate <= 0.0 || total <= 0) {
        error = "empty audio";
        return false;
    }
    if (total < static_cast<std::int64_t>(kWindow)) {
        error = "audio shorter than one spectrum frame (8192 samples)";
        return false;
    }
    const std::uint32_t src_ch = source.channels();
    FrameAnalyser analyser(rate);
    std::vector<Frame> frames;
    std::vector<std::vector<double>> pending(nch);
    std::vector<std::vector<double>> win(nch, std::vector<double>(kWindow));
    const std::int64_t block = std::max<std::uint32_t>(run.block_frames, 1u);
    std::vector<float> buf(static_cast<std::size_t>(block) * src_ch);
    for (std::int64_t pos = 0; pos < total;) {
        if (run.cancel && run.cancel->load()) {
            error = "cancelled";
            return false;
        }
        const std::int64_t got = source.read(pos, std::min(block, total - pos), buf.data());
        if (got < 0) {
            error = "read failed";
            return false;
        }
        if (got == 0) break;
        for (std::int64_t i = 0; i < got; ++i)
            for (std::uint32_t c = 0; c < nch; ++c)
                pending[c].push_back(buf[static_cast<std::size_t>(i) * src_ch + c]);
        std::size_t used = 0;
        while (pending[0].size() - used >= kWindow) {
            for (std::uint32_t c = 0; c < nch; ++c)
                std::copy(pending[c].begin() + static_cast<std::ptrdiff_t>(used),
                          pending[c].begin() + static_cast<std::ptrdiff_t>(used + kWindow), win[c].begin());
            frames.emplace_back();
            analyser.analyse(win, frames.back());
            used += kHop;
        }
        for (auto& p : pending) p.erase(p.begin(), p.begin() + static_cast<std::ptrdiff_t>(used));
        pos += got;
        if (run.progress) run.progress(static_cast<double>(pos) / static_cast<double>(total));
    }

    // Gate: absolute, then relative to the mean power of what passed.
    const double abs_gate = std::pow(10.0, kAbsoluteGateDb / 10.0);
    double sum = 0.0;
    int n = 0;
    for (const Frame& f : frames)
        if (f.power / nch > abs_gate) {
            sum += f.power;
            ++n;
        }
    if (n == 0) {
        error = "audio is silent (nothing above -70 dBFS)";
        return false;
    }
    const double rel_gate = (sum / n) * std::pow(10.0, kRelativeGateDb / 10.0);
    std::array<double, kTimbreBands> acc{};
    int kept = 0;
    for (const Frame& f : frames) {
        if (f.power / nch <= abs_gate || f.power <= rel_gate) continue;
        ++kept;
        for (int b = 0; b < kTimbreBands; ++b) acc[b] += f.band[b];
    }

    // Level-normalise to the total energy (density times bandwidth, summed
    // over the usable bands) so a level difference is not matched.
    double energy = 0.0;
    for (int b = 0; b < kTimbreBands; ++b) {
        out.valid[b] = analyser.usable(b) && acc[b] > 0.0;
        if (out.valid[b]) energy += acc[b] / kept * kThirdOctaveCentreHz[b];
    }
    if (energy <= 0.0) {
        error = "no usable band";
        return false;
    }
    const double ref_db = 10.0 * std::log10(energy);
    for (int b = 0; b < kTimbreBands; ++b)
        out.gain_db[b] = out.valid[b] ? 10.0 * std::log10(acc[b] / kept) - ref_db : 0.0;
    return true;
}

}  // namespace sonora::analyze
