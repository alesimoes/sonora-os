// SPDX-License-Identifier: GPL-3.0-or-later
// The draft plugin channel layout: offsets agree with the header, the region
// fits, a forged header is rejected, and the parameter ring never overfills.
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

    ChannelHeader* h = init_header(base, 48000, frames, 128, ch, 1, 3, 70);
    CHECK(header_valid(*h, size));
    CHECK(!header_valid(*h, size - 1));  // region smaller than it claims
    CHECK(h->input_offset % kCacheLine == 0);
    CHECK(h->output_offset - h->input_offset >= std::size_t(ch) * frames * sizeof(float));
    CHECK(reinterpret_cast<unsigned char*>(output_plane(base, *h, ch - 1) + frames) <=
          static_cast<unsigned char*>(base) + size);
    CHECK(h->current_instance.load() == -1);

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

    if (failures) return EXIT_FAILURE;
    std::printf("plugin protocol layout: ok (region %zu bytes for %u ch x %u frames)\n", size,
                ch, frames);
    return EXIT_SUCCESS;
}
