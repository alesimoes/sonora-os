// SPDX-License-Identifier: GPL-3.0-or-later
//
// Onsets from the waveform overview: the loudest of the region's channels
// per bucket, a hit where it rises past a threshold set below the region's
// own peak, re-armed only once it has fallen well below. Relative to the
// region, so a quiet reference finds its hits as a loud one does.
#include <algorithm>
#include <cmath>

#include "peak_source.h"
#include "sonora/ui_groove/onset_source.h"

namespace sonora::ui {

using model::Frames;

namespace {

double gain_of(double db) { return std::pow(10.0, db / 20.0); }

}  // namespace

bool PeakOnsetSource::onsets(const model::Region& r, std::uint32_t sample_rate,
                             std::vector<Frames>& out, std::string& why) const {
    out.clear();
    for (const model::SourceRef& c : r.channels) {
        if (!peaks_.ready(c.source)) {
            why = "the waveform overview of " + r.name +
                  " is still being read; try again in a moment. Nothing was moved.";
            return false;
        }
    }
    const double fpb = std::max(1.0, params_.bucket_ms * sample_rate / 1000.0);
    const int count = static_cast<int>(std::ceil(static_cast<double>(r.length) / fpb));
    if (count <= 0 || r.channels.empty()) {
        why = r.name + " holds no audio to take a groove from";
        return false;
    }
    std::vector<float> env(static_cast<std::size_t>(count), 0.0f);
    std::vector<PeakPair> buf(static_cast<std::size_t>(count));
    for (const model::SourceRef& c : r.channels) {
        peaks_.peaks(c.source, c.channel, r.source_offset, fpb, count, buf.data());
        for (int i = 0; i < count; ++i) {
            const auto& p = buf[static_cast<std::size_t>(i)];
            env[static_cast<std::size_t>(i)] =
                std::max(env[static_cast<std::size_t>(i)], std::max(-p.min, p.max));
        }
    }
    const float loudest = *std::max_element(env.begin(), env.end());
    if (!(loudest > 0.0f)) {
        why = r.name + " is silent; there is nothing to take a groove from";
        return false;
    }
    const double on = loudest * gain_of(params_.threshold_db);
    const double off = loudest * gain_of(params_.release_db);
    const auto gap = static_cast<Frames>(params_.min_gap_ms * sample_rate / 1000.0);
    bool armed = true;
    for (int i = 0; i < count; ++i) {
        const double v = env[static_cast<std::size_t>(i)];
        const Frames at = r.position + static_cast<Frames>(std::llround(i * fpb));
        if (armed && v >= on) {
            if (out.empty() || at - out.back() >= gap) out.push_back(at);
            armed = false;
        } else if (!armed && v <= off) {
            armed = true;
        }
    }
    return true;
}

}  // namespace sonora::ui
