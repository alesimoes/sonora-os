// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <utility>

#include "sonora/pitch/shifter.h"

namespace sonora::pitch {

bool FakePitchShifter::shift(const Planar& input, uint32_t rate, const api::PitchEditSpec& spec,
                             Planar& output, std::string& why) {
    if (input.empty() || input[0].empty()) {
        why = "pitch: no audio to shift";
        return false;
    }
    const std::size_t frames = input[0].size();
    for (const std::vector<float>& ch : input) {
        if (ch.size() != frames) {
            why = "pitch: the channels differ in length";
            return false;
        }
    }
    if (!refusal_.empty()) {
        why = refusal_;
        return false;
    }
    if (!api::validate_pitch_spec(spec, why) || !api::pitch_notes_fit(spec, frames, why)) return false;

    calls_.push_back(Call{input.size(), frames, rate, spec});
    Planar out = input;
    for (const api::PitchNote& n : spec.notes) {
        const float gain = float(std::pow(10.0, n.gain_db / 20.0));
        for (std::vector<float>& ch : out) {
            for (uint64_t i = n.start; i < n.end; ++i) ch[std::size_t(i)] *= gain;
        }
    }
    output = std::move(out);
    return true;
}

}  // namespace sonora::pitch
