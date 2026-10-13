// SPDX-License-Identifier: GPL-3.0-or-later
// The draft plugin channel layout: offsets agree with the header, the region
// fits, a forged header is rejected, the parameter ring never overfills, and
// nothing the host writes can move where the engine reads or writes.
#include "sonora/plugin/protocol.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace sonora::plugin;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

int main() {
    const uint32_t ch = 18, frames = 1024;
    const std::size_t size = region_size(ch, frames);
    std::vector<unsigned char> storage(size + kCacheLine);
    const std::size_t pad =
        (kCacheLine - reinterpret_cast<std::uintptr_t>(storage.data()) % kCacheLine) % kCacheLine;
    void* base = storage.data() + pad;
    std::memset(base, 0, size);

    const Layout layout = make_layout(ch, frames);
    ChannelHeader* h = init_header(base, layout, 48000, 128, 1, 3, 70);
    CHECK(header_valid(*h, size));
    CHECK(!header_valid(*h, size - 1));  // region smaller than it claims
    CHECK(layout.region_size == size);
    CHECK(h->input_offset == layout.input_offset && h->output_offset == layout.output_offset);
    CHECK(layout.input_offset % kCacheLine == 0);
    CHECK(layout.output_offset - layout.input_offset >= std::size_t(ch) * frames * sizeof(float));
    CHECK(reinterpret_cast<unsigned char*>(output_plane(base, layout, ch - 1) + frames) <=
          static_cast<unsigned char*>(base) + size);
    CHECK(h->current_instance.load() == -1);
    CHECK(header_valid(*h, size) && layout_of(*h).region_size == layout.region_size);

    // A host that scribbles over the fixed part of the header (it is mapped
    // writable) must not move the engine's planes: the engine computes them
    // from its own copy of the layout, never from the shared words.
    float* const in_before = input_plane(base, layout, 3);
    float* const out_before = output_plane(base, layout, 3);
    const uint64_t saved_in = h->input_offset, saved_out = h->output_offset;
    const uint32_t saved_frames = h->max_frames;
    h->input_offset = 0xFFFFFFFFFFFF0000ull;
    h->output_offset = 8;
    h->max_frames = 0x7FFFFFFF;
    CHECK(input_plane(base, layout, 3) == in_before);
    CHECK(output_plane(base, layout, 3) == out_before);
    CHECK(!header_valid(*h, size));  // and a host given such a header refuses it
    h->input_offset = saved_in;
    h->output_offset = saved_out;
    h->max_frames = saved_frames;
    CHECK(header_valid(*h, size));

    // Host-written words are bounds-checked before the engine uses them.
    CHECK(checked_instance(-1, 3) == -1);
    CHECK(checked_instance(2, 3) == 2);
    CHECK(checked_instance(3, 3) == -1);
    CHECK(checked_instance(0x7FFFFFFF, 3) == -1);
    CHECK(checked_instance(-7, 3) == -1);
    CHECK(checked_latency(304) == 304);
    CHECK(checked_latency(kMaxLatencyFrames) == kMaxLatencyFrames);
    CHECK(checked_latency(kMaxLatencyFrames + 1) == kLatencyInvalid);
    CHECK(checked_state(uint32_t(HostState::Ready)) == HostState::Ready);
    CHECK(checked_state(99) == HostState::Faulted);

    // Words written by different processes must not share a cache line.
    const auto line = [&](const void* p) {
        return std::size_t(static_cast<const unsigned char*>(p) -
                           static_cast<unsigned char*>(base)) / kCacheLine;
    };
    CHECK(line(&h->submitted_block) != line(&h->completed_block));
    CHECK(line(&h->param_write) != line(&h->param_read));

    h->magic ^= 1;
    CHECK(!header_valid(*h, size));
    h->magic ^= 1;
    h->pipeline_depth = 2;
    CHECK(!header_valid(*h, size));
    h->pipeline_depth = 1;
    CHECK(header_valid(*h, size));

    // Ring: fills to capacity exactly, refuses the next, drains in order.
    for (uint32_t i = 0; i < kParamRingCapacity; ++i) CHECK(push_param(*h, {0, i, float(i), 0}));
    CHECK(!push_param(*h, {0, 999, 0.f, 0}));
    ParamChange p{};
    for (uint32_t i = 0; i < kParamRingCapacity; ++i) {
        CHECK(pop_param(*h, p));
        CHECK(p.port == i);
    }
    CHECK(!pop_param(*h, p));

    // A host that scribbles param_read cannot make the engine write outside
    // the ring: the index is masked, and a read index ahead of the write
    // index reads as a full ring, so pushes are refused and counted.
    h->param_read.store(h->param_write.load() + 5);
    CHECK(!push_param(*h, {0, 1, 1.f, 0}));
    h->param_read.store(h->param_write.load() - 3);
    CHECK(push_param(*h, {0, 1, 1.f, 0}));

    if (failures) return EXIT_FAILURE;
    std::printf("plugin protocol layout: ok (region %zu bytes for %u ch x %u frames)\n", size,
                ch, frames);
    return EXIT_SUCCESS;
}
