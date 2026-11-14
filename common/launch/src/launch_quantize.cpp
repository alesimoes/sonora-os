#include "sonora/launch/launch_quantize.h"

#include <cmath>

namespace sonora::launch {

namespace {

double beats_per_step(const TransportSnapshot& t, Quantize q) {
    const double bar = static_cast<double>(t.beats_per_bar);
    switch (q) {
    case Quantize::Immediate: return 0.0;
    case Quantize::Beat: return 1.0;
    case Quantize::Bar: return bar;
    case Quantize::Bars2: return 2.0 * bar;
    case Quantize::Bars4: return 4.0 * bar;
    }
    return 0.0;
}

}  // namespace

double step_frames(const TransportSnapshot& t, Quantize q) {
    if (!(t.sample_rate > 0.0) || !(t.tempo_bpm > 0.0) || t.beats_per_bar == 0) return 0.0;
    return beats_per_step(t, q) * t.sample_rate * 60.0 / t.tempo_bpm;
}

std::int64_t boundary_frame(const TransportSnapshot& t, Quantize q, std::int64_t k) {
    const double offset = static_cast<double>(k) * step_frames(t, q);
    // floor(x + 0.5), not llround: no errno or FE flags on the audio thread.
    return t.anchor_frame + static_cast<std::int64_t>(std::floor(offset + 0.5));
}

std::int64_t next_boundary(std::int64_t pos_frames, const TransportSnapshot& t, Quantize q) {
    const double step = step_frames(t, q);
    if (!(step > 0.0)) return pos_frames;
    std::int64_t k = static_cast<std::int64_t>(
        std::floor(static_cast<double>(pos_frames - t.anchor_frame) / step));
    // The estimate is off by at most one either way after rounding; settle it
    // against boundary_frame so every caller agrees on the same bar lines.
    while (boundary_frame(t, q, k) < pos_frames) ++k;
    while (boundary_frame(t, q, k - 1) >= pos_frames) --k;
    return boundary_frame(t, q, k);
}

}  // namespace sonora::launch
