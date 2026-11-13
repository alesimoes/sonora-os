// advance() on the audio thread must not allocate (ADR-0164, CLAUDE.md).
// The global allocator is replaced with a counter; every advance() call,
// command draining included, runs with the count armed.
#include "check.h"
#include "sonora/launch/launch_clock.h"
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

    // The callback's driver, as the engine runs it: stopped, rolling, a loop
    // that wraps mid-block, and a tempo change.
    LaunchClock clock(l);
    BlockTransport tp;
    tp.loop_start = 1000;
    tp.loop_end = 1000 + 3 * 48000 + 77;
    std::uint32_t transitions = 0;
    std::uint32_t captures = 0;
    for (int round = 0; round < 40; ++round) {
        // Producer side between blocks; only process() is measured.
        l.trigger_scene(round % 8);
        l.trigger_cell(round % 16, (round + 3) % 8);
        l.arm_record(round % 16, round % 8, 1000 + round, round % 3);
        if (round % 5 == 0) l.stop_all();
        if (round % 7 == 0) l.set_quantize(round % 2 ? Quantize::Immediate : Quantize::Beat);
        tp.rolling = round % 4 != 0;
        tp.looping = round % 3 == 0;
        tp.tempo_bpm = round % 6 == 0 ? 97.0 : 120.0;
        for (int b = 0; b < 200; ++b) {
            g_armed.store(true);
            const TransitionList list = clock.process(tp, 256);
            g_armed.store(false);
            transitions += list.count;
            for (std::uint32_t i = 0; i < list.count; ++i)
                if (list[i].kind == Transition::Kind::RecordStart) ++captures;
            CHECK(list.dropped == 0);
            if (tp.rolling) {
                tp.position += 256;
                if (tp.looping && tp.position >= tp.loop_end)
                    tp.position = tp.loop_start + (tp.position - tp.loop_end);
            }
        }
    }
    CHECK(transitions > 0);
    CHECK(captures > 0);
    CHECK(g_allocs.load() == 0);
    // No lock either: the only shared state the audio thread touches.
    CHECK(l.clock_is_lock_free());
    std::printf("launch_no_alloc: ok (%u transitions)\n", transitions);
    return 0;
}
