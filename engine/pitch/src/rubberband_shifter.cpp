// SPDX-License-Identifier: GPL-3.0-or-later
#include "rubberband_shifter.h"

#include <rubberband/RubberBandStretcher.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace sonora {

namespace {

using RubberBand::RubberBandStretcher;

// R3 (the finer engine) and its formant scale came with API 2.7.
#if RUBBERBAND_API_MAJOR_VERSION > 2 || \
    (RUBBERBAND_API_MAJOR_VERSION == 2 && RUBBERBAND_API_MINOR_VERSION >= 7)
#define SONORA_RUBBERBAND_R3 1
#else
#define SONORA_RUBBERBAND_R3 0
#endif

// Audio either side of a note handed to the stretcher, so its analysis has
// settled by the time the note starts.
constexpr std::size_t kContext = 4096;
// The crossfade into and out of a note, outside it: 5 ms at 48 kHz.
constexpr std::size_t kFade = 256;
constexpr std::size_t kBlock = 4096;

double cents_to_scale(int32_t cents) { return std::pow(2.0, cents / 1200.0); }

// input[c][c0, c1) shifted by `note`, into seg (c1 - c0 frames per channel).
bool render_segment(const pitch::Planar& input, std::size_t c0, std::size_t c1, uint32_t rate,
                    const api::PitchNote& note, pitch::Planar& seg, std::string& why) {
    const std::size_t nch = input.size();
    const std::size_t len = c1 - c0;
    // Threads would leave output pending after the final block; offline the
    // caller's thread is the one to use.
    int options = RubberBandStretcher::OptionProcessOffline |
                  RubberBandStretcher::OptionThreadingNever |
                  RubberBandStretcher::OptionFormantPreserved;
#if SONORA_RUBBERBAND_R3
    options |= RubberBandStretcher::OptionEngineFiner;
#else
    if (note.formant_cents != 0) {
        why = "pitch: this Rubber Band cannot move formants (it needs version 3)";
        return false;
    }
#endif
    RubberBandStretcher rb(rate, nch, options, 1.0, cents_to_scale(note.cents));
#if SONORA_RUBBERBAND_R3
    if (note.formant_cents != 0) rb.setFormantScale(cents_to_scale(note.formant_cents));
#endif
    rb.setExpectedInputDuration(len);
    rb.setMaxProcessSize(kBlock);

    std::vector<const float*> in(nch);
    for (std::size_t pos = 0; pos < len; pos += kBlock) {
        const std::size_t n = std::min(kBlock, len - pos);
        for (std::size_t c = 0; c < nch; ++c) in[c] = input[c].data() + c0 + pos;
        rb.study(in.data(), n, pos + n >= len);
    }
    seg.assign(nch, std::vector<float>(len, 0.0f));
    std::vector<std::vector<float>> got(nch, std::vector<float>(kBlock));
    std::vector<float*> outp(nch);
    std::size_t written = 0;
    const auto drain = [&]() {
        for (int avail = rb.available(); avail > 0; avail = rb.available()) {
            const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(avail), kBlock);
            for (std::size_t c = 0; c < nch; ++c) outp[c] = got[c].data();
            const std::size_t r = rb.retrieve(outp.data(), n);
            if (r == 0) return;
            const std::size_t keep = written < len ? std::min(r, len - written) : 0;
            for (std::size_t c = 0; c < nch; ++c) {
                std::copy(got[c].begin(), got[c].begin() + static_cast<std::ptrdiff_t>(keep),
                          seg[c].begin() + static_cast<std::ptrdiff_t>(written));
            }
            written += keep;
        }
    };
    for (std::size_t pos = 0; pos < len; pos += kBlock) {
        const std::size_t n = std::min(kBlock, len - pos);
        for (std::size_t c = 0; c < nch; ++c) in[c] = input[c].data() + c0 + pos;
        rb.process(in.data(), n, pos + n >= len);
        drain();
    }
    drain();
    // A shortfall of a few frames at the very end stays silent, inside the
    // context, and is faded out of the note below.
    if (written + kContext < len) {
        why = "pitch: Rubber Band returned " + std::to_string(written) + " of " +
              std::to_string(len) + " frames";
        return false;
    }
    return true;
}

// The weight of the shifted audio at frame i: 1 inside the note, ramping to 0
// over kFade frames outside it.
float weight_at(std::size_t i, std::size_t start, std::size_t end) {
    if (i >= start && i < end) return 1.0f;
    if (i < start) return 1.0f - static_cast<float>(start - i) / static_cast<float>(kFade + 1);
    return 1.0f - static_cast<float>(i - end + 1) / static_cast<float>(kFade + 1);
}

}  // namespace

bool RubberBandPitchShifter::shift(const pitch::Planar& input, uint32_t rate,
                                   const api::PitchEditSpec& spec, pitch::Planar& output,
                                   std::string& why) {
    if (input.empty() || input[0].empty() || rate == 0) {
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
    if (!api::validate_pitch_spec(spec, why) || !api::pitch_notes_fit(spec, frames, why)) return false;

    pitch::Planar out = input;
    pitch::Planar seg;
    for (const api::PitchNote& n : spec.notes) {
        const auto start = static_cast<std::size_t>(n.start);
        const auto end = static_cast<std::size_t>(n.end);
        const float gain = static_cast<float>(std::pow(10.0, n.gain_db / 20.0));
        if (n.cents == 0 && n.formant_cents == 0) {
            for (std::vector<float>& ch : out) {
                for (std::size_t i = start; i < end; ++i) ch[i] *= gain;
            }
            continue;
        }
        const std::size_t c0 = start > kContext ? start - kContext : 0;
        const std::size_t c1 = std::min(frames, end + kContext);
        if (!render_segment(input, c0, c1, rate, n, seg, why)) return false;
        const std::size_t f0 = start > kFade ? start - kFade : 0;
        const std::size_t f1 = std::min(frames, end + kFade);
        for (std::size_t c = 0; c < out.size(); ++c) {
            for (std::size_t i = f0; i < f1; ++i) {
                const float w = weight_at(i, start, end);
                const float g = (i >= start && i < end) ? gain : 1.0f;
                out[c][i] = out[c][i] * (1.0f - w) + seg[c][i - c0] * w * g;
            }
        }
    }
    output = std::move(out);
    return true;
}

}  // namespace sonora
