// Sample-exact boundaries across block edges (ADR-0164): whatever the block
// size, a quantised transition lands on the boundary frame, in exactly one
// block, at offset boundary - block_start. Blocks are half-open: a boundary
// on a block's first frame belongs to that block, not the one before.
#include "check.h"
#include "sonora/launch/state_machine_launcher.h"

using namespace sonora::launch;

namespace {

struct Hit {
    int count = 0;
    std::int64_t frame = -1;
    std::uint32_t offset = 0;
    std::uint32_t frames = 0;
};

// One track, one clip, triggered at `trigger_at`, then advanced in blocks
// whose sizes cycle through `sizes`. Reports the start transitions seen.
Hit start_hits(const TransportSnapshot& t, Quantize q, std::int64_t trigger_at,
               const std::uint32_t* sizes, int n_sizes, std::int64_t until) {
    LauncherConfig c;
    c.tracks = 1;
    c.scenes = 1;
    c.quantize = q;
    StateMachineLauncher l(c);
    l.set_transport(t);
    l.assign_clip(0, 0, 7);
    l.trigger_cell(0, 0);
    Hit hit;
    std::int64_t pos = trigger_at;
    for (int i = 0; pos < until; ++i) {
        const std::uint32_t frames = sizes[i % n_sizes];
        const TransitionList list = l.advance(pos, frames);
        for (std::uint32_t k = 0; k < list.count; ++k) {
            if (list[k].kind != Transition::Kind::Start) continue;
            ++hit.count;
            hit.frame = pos + list[k].offset;
            hit.offset = list[k].offset;
            hit.frames = frames;
        }
        pos += frames;
    }
    return hit;
}

int test_boundary_on_first_and_last_frame() {
    const TransportSnapshot t{48000.0, 120.0, 4, 0};
    // 96000 = 375 * 256: the boundary is the first frame of a block.
    const std::uint32_t s256[] = {256};
    Hit h = start_hits(t, Quantize::Bar, 256, s256, 1, 200000);
    CHECK(h.count == 1 && h.frame == 96000 && h.offset == 0);
    // Blocks of 1000 from 1 put 96000 at offset 999 of the block at 95001.
    const std::uint32_t s1000[] = {1000};
    h = start_hits(t, Quantize::Bar, 1, s1000, 1, 200000);
    CHECK(h.count == 1 && h.frame == 96000 && h.offset == 999 && h.frames == 1000);
    return 0;
}

int test_irregular_blocks() {
    const TransportSnapshot t{48000.0, 120.0, 4, 0};
    const std::uint32_t sizes[] = {37, 512, 1, 997, 64, 2048, 3};
    for (std::int64_t trigger = 1; trigger < 5000; trigger += 389) {
        const Hit h = start_hits(t, Quantize::Bar, trigger, sizes, 7, 200000);
        CHECK(h.count == 1);
        CHECK(h.frame == 96000);
        CHECK(h.offset < h.frames);
    }
    return 0;
}

int test_fractional_tempo_every_block_size() {
    // 44.1 kHz at 123 BPM: no whole-frame beat; the start must still land on
    // the frame boundary_frame() names, for every block size tried.
    const TransportSnapshot t{44100.0, 123.0, 4, 0};
    const std::int64_t expected = boundary_frame(t, Quantize::Beat, 1);
    for (std::uint32_t size = 1; size <= 4096; size = size * 3 + 1) {
        const std::uint32_t s[] = {size};
        const Hit h = start_hits(t, Quantize::Beat, 5, s, 1, expected + 8192);
        CHECK(h.count == 1);
        CHECK(h.frame == expected);
    }
    return 0;
}

int test_anchor_offset() {
    const TransportSnapshot t{48000.0, 120.0, 4, 4321};
    const std::uint32_t s[] = {480};
    const Hit h = start_hits(t, Quantize::Bars2, 4322, s, 1, 400000);
    CHECK(h.count == 1 && h.frame == 4321 + 192000);
    return 0;
}

}  // namespace

int main() {
    if (test_boundary_on_first_and_last_frame()) return 1;
    if (test_irregular_blocks()) return 1;
    if (test_fractional_tempo_every_block_size()) return 1;
    if (test_anchor_offset()) return 1;
    std::printf("launch_block_edges: ok\n");
    return 0;
}
