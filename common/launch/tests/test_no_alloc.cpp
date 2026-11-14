// advance() on the audio thread must not allocate (ADR-0164, CLAUDE.md).
// The global allocator is replaced with a counter; every advance() call,
// command draining included, runs with the count armed.
#include "check.h"
#include "sonora/launch/state_machine_launcher.h"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {
std::atomic<long> g_allocs{0};
std::atomic<bool> g_armed{false};
}  // namespace

void* operator new(std::size_t n) {
    if (g_armed.load(std::memory_order_relaxed)) g_allocs.fetch_add(1);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
    if (g_armed.load(std::memory_order_relaxed)) g_allocs.fetch_add(1);
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }

using namespace sonora::launch;

int main() {
    LauncherConfig c;
    c.tracks = 16;
    c.scenes = 8;
    c.queue_capacity = 128;
    StateMachineLauncher l(c);
    l.set_transport(TransportSnapshot{48000.0, 120.0, 4, 0});
    for (std::uint32_t t = 0; t < 16; ++t)
        for (std::uint32_t s = 0; s < 8; ++s) l.assign_clip(t, s, 1 + t * 8 + s);

    std::int64_t pos = 0;
    std::uint32_t transitions = 0;
    for (int round = 0; round < 40; ++round) {
        // Producer side between blocks; only advance() is measured.
        l.trigger_scene(round % 8);
        l.trigger_cell(round % 16, (round + 3) % 8);
        if (round % 5 == 0) l.stop_all();
        if (round % 7 == 0) l.set_quantize(round % 2 ? Quantize::Immediate : Quantize::Beat);
        for (int b = 0; b < 200; ++b) {
            g_armed.store(true);
            const TransitionList list = l.advance(pos, 256);
            g_armed.store(false);
            transitions += list.count;
            CHECK(list.dropped == 0);
            pos += 256;
        }
    }
    CHECK(transitions > 0);
    CHECK(g_allocs.load() == 0);
    std::printf("launch_no_alloc: ok (%u transitions)\n", transitions);
    return 0;
}
