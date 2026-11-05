// SPDX-License-Identifier: GPL-3.0-or-later
//
// Offline conditioning of an impulse response before it is loaded into a
// convolution kernel: trim leading and trailing near-silence, and normalise
// the level. Control thread only (it works on a std::vector); never reached
// from process(). Trimming the head removes the dead time most cabinet
// captures carry, a fixed delay the player would otherwise hear.
//
// Normalisation is to the energy (sum of squares) and not the peak: two
// cabinet responses of equal loudness differ little in energy, but their
// peaks differ by the sharpness of the first reflection. The gain is capped
// so a near-silent file is not blown up.
#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace sonora::dsp {

struct IrPrepare {
    bool trim = false;
    double head_db = -60.0;      // leading samples below this, re peak, are dropped
    double tail_db = -80.0;      // trailing samples below this, re peak, are dropped
    bool normalise = false;
    double target_energy = 1.0;  // sum of squares after normalising
    double max_gain_db = 40.0;
};

// Returns false (and leaves `ir` untouched) when the response is silent.
inline bool prepare_ir(std::vector<float>& ir, const IrPrepare& o) {
    double peak = 0.0;
    for (float v : ir) peak = std::fmax(peak, std::fabs(static_cast<double>(v)));
    if (!(peak > 0.0)) return false;
    if (o.trim) {
        const double hi = peak * std::pow(10.0, o.head_db / 20.0);
        const double ti = peak * std::pow(10.0, o.tail_db / 20.0);
        std::size_t first = 0, last = ir.size();
        while (first < ir.size() && std::fabs(static_cast<double>(ir[first])) < hi) ++first;
        while (last > first + 1 && std::fabs(static_cast<double>(ir[last - 1])) < ti) --last;
        ir.erase(ir.begin() + static_cast<std::ptrdiff_t>(last), ir.end());
        ir.erase(ir.begin(), ir.begin() + static_cast<std::ptrdiff_t>(first));
    }
    if (o.normalise) {
        double e = 0.0;
        for (float v : ir) e += static_cast<double>(v) * static_cast<double>(v);
        if (!(e > 0.0)) return false;
        const double cap = std::pow(10.0, o.max_gain_db / 20.0);
        double g = std::sqrt(o.target_energy / e);
        if (g > cap) g = cap;
        for (float& v : ir) v = static_cast<float>(static_cast<double>(v) * g);
    }
    return true;
}

}  // namespace sonora::dsp
