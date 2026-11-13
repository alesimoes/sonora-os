// Launch quantisation (ADR-0164): boundaries at 120 BPM 4/4 48 kHz, an anchor
// offset, positions before the anchor, and a tempo with no whole-frame beat.
#include "check.h"
#include "sonora/launch/launch_quantize.h"

using namespace sonora::launch;

namespace {

// 120 BPM 4/4 at 48 kHz: a beat is 24000 frames, a bar 96000.
TransportSnapshot t120() { return TransportSnapshot{48000.0, 120.0, 4, 0}; }

int test_grid_steps() {
    const TransportSnapshot t = t120();
    CHECK(next_boundary(0, t, Quantize::Bar) == 0);
    CHECK(next_boundary(1, t, Quantize::Bar) == 96000);
    CHECK(next_boundary(95999, t, Quantize::Bar) == 96000);
    CHECK(next_boundary(96000, t, Quantize::Bar) == 96000);
    CHECK(next_boundary(96001, t, Quantize::Bar) == 192000);
    CHECK(next_boundary(1, t, Quantize::Beat) == 24000);
    CHECK(next_boundary(24001, t, Quantize::Beat) == 48000);
    CHECK(next_boundary(1, t, Quantize::Bars2) == 192000);
    CHECK(next_boundary(192001, t, Quantize::Bars4) == 384000);
    CHECK(next_boundary(12345, t, Quantize::Immediate) == 12345);
    return 0;
}

int test_anchor_and_negative() {
    TransportSnapshot t = t120();
    t.anchor_frame = 1000;
    CHECK(next_boundary(0, t, Quantize::Bar) == 1000);
    CHECK(next_boundary(1001, t, Quantize::Bar) == 97000);
    CHECK(next_boundary(-200000, t, Quantize::Bar) == 1000 - 192000);
    CHECK(next_boundary(-95000, t, Quantize::Bar) == -95000);
    CHECK(next_boundary(-94999, t, Quantize::Bar) == 1000);
    return 0;
}

int test_invalid_snapshot_is_immediate() {
    TransportSnapshot t = t120();
    t.tempo_bpm = 0.0;
    CHECK(next_boundary(777, t, Quantize::Bar) == 777);
    t = t120();
    t.beats_per_bar = 0;
    CHECK(next_boundary(777, t, Quantize::Bar) == 777);
    return 0;
}

// 44.1 kHz at 123 BPM: a beat is 21512.195... frames. Every position must
// map to the boundary that boundary_frame() names, with no drift far out.
int test_fractional_step() {
    const TransportSnapshot t{44100.0, 123.0, 3, 0};
    for (std::int64_t k = 0; k < 2000; k += 7) {
        const std::int64_t b = boundary_frame(t, Quantize::Beat, k);
        CHECK(next_boundary(b, t, Quantize::Beat) == b);
        CHECK(next_boundary(b + 1, t, Quantize::Beat) == boundary_frame(t, Quantize::Beat, k + 1));
        CHECK(next_boundary(b - 1, t, Quantize::Beat) == b);
    }
    // Bar 1000 (3 beats each) sits where 3000 beats do, to the frame.
    CHECK(boundary_frame(t, Quantize::Bar, 1000) == boundary_frame(t, Quantize::Beat, 3000));
    return 0;
}

}  // namespace

int main() {
    if (test_grid_steps()) return 1;
    if (test_anchor_and_negative()) return 1;
    if (test_invalid_snapshot_is_immediate()) return 1;
    if (test_fractional_step()) return 1;
    std::printf("launch_quantize: ok\n");
    return 0;
}
