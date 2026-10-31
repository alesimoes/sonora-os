// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared by engine/core's playback tests: the CHECK macro, sources written
// with known contents, plans built by hand, and, for a test that defines
// PLAYBACK_TEST_COUNT_HEAP before including this, a heap counter for the
// thread inside Session::process(). engine/control builds real plans from a
// model; these tests are about what engine/core does with one.
#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <new>
#include <string>
#include <vector>

#include "sonora/plan_graph.h"
#include "sonora/playback_plan.h"
#include "sonora/session.h"
#include "sonora/wav_writer.h"

// The counter replaces the global allocators, so it is opt-in, and a test
// executable that opts in includes this from one translation unit only.
#if defined(PLAYBACK_TEST_COUNT_HEAP)
// Every way process() could reach the heap is counted on the thread that
// sets t_counting: each operator new and delete overload, and, outside the
// sanitizers (whose own interceptors must keep malloc), malloc, calloc,
// realloc, the aligned allocators and free. thread_local: a test's other
// threads (the disk reader, a host supervisor) allocate freely, uncounted.
namespace playback_test {
thread_local bool t_counting = false;
std::atomic<long> g_allocations{0};
std::atomic<long> g_frees{0};

inline void note_alloc() {
    if (t_counting) g_allocations.fetch_add(1, std::memory_order_relaxed);
}
inline void note_free(void* p) {
    if (t_counting && p != nullptr) g_frees.fetch_add(1, std::memory_order_relaxed);
}
inline void reset_heap_counts() {
    g_allocations.store(0);
    g_frees.store(0);
}
inline bool heap_untouched() { return g_allocations.load() == 0 && g_frees.load() == 0; }
}  // namespace playback_test

// RTSan interposes malloc/free itself (a weak trampoline); a strong
// definition here would replace it and blind RTSan's heap checks in every
// test that includes this header.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer) ||     __has_feature(realtime_sanitizer)
#define PLAYBACK_TEST_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define PLAYBACK_TEST_SANITIZED 1
#endif

#if defined(__GLIBC__) && !defined(PLAYBACK_TEST_SANITIZED)
#define PLAYBACK_TEST_COUNTS_MALLOC 1
extern "C" {
void* __libc_malloc(std::size_t);
void* __libc_calloc(std::size_t, std::size_t);
void* __libc_realloc(void*, std::size_t);
void* __libc_memalign(std::size_t, std::size_t);
void __libc_free(void*);

// noexcept on each, as glibc's __THROW declares them. GCC excuses a missing
// specifier only when the system declaration comes first; <malloc.h>, and
// <mm_malloc.h> (posix_memalign, reached from <random> when SSE3 is on),
// redeclare these after this header and make a mismatch a hard error.
// test_heap_counter builds that order.
void* malloc(std::size_t n) noexcept {
    playback_test::note_alloc();
    return __libc_malloc(n);
}
void* calloc(std::size_t n, std::size_t size) noexcept {
    playback_test::note_alloc();
    return __libc_calloc(n, size);
}
void* realloc(void* p, std::size_t n) noexcept {
    playback_test::note_alloc();
    return __libc_realloc(p, n);
}
void* aligned_alloc(std::size_t align, std::size_t n) noexcept {
    playback_test::note_alloc();
    return __libc_memalign(align, n);
}
void* memalign(std::size_t align, std::size_t n) noexcept {
    playback_test::note_alloc();
    return __libc_memalign(align, n);
}
int posix_memalign(void** out, std::size_t align, std::size_t n) noexcept {
    playback_test::note_alloc();
    void* p = __libc_memalign(align, n);
    if (p == nullptr) return 12;  // ENOMEM
    *out = p;
    return 0;
}
void free(void* p) noexcept {
    playback_test::note_free(p);
    __libc_free(p);
}
}
#endif

namespace playback_test {
inline void* counted_new(std::size_t size) {
    note_alloc();
    return std::malloc(size > 0 ? size : 1);
}
inline void* counted_new_aligned(std::size_t size, std::align_val_t align) {
    note_alloc();
    void* p = nullptr;
    const std::size_t a = static_cast<std::size_t>(align) < sizeof(void*)
                              ? sizeof(void*)
                              : static_cast<std::size_t>(align);
    return posix_memalign(&p, a, size > 0 ? size : 1) == 0 ? p : nullptr;
}
inline void counted_delete(void* p) {
    note_free(p);
    std::free(p);
}

// What the counter covers in this build, for a test's evidence line.
inline const char* heap_counter_scope() {
#if defined(PLAYBACK_TEST_COUNTS_MALLOC)
    return "operator new/delete and malloc/calloc/realloc/aligned/free";
#else
    return "operator new/delete only (a sanitizer or non-glibc build keeps malloc)";
#endif
}
}  // namespace playback_test

void* operator new(std::size_t size) {
    if (void* p = playback_test::counted_new(size)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return playback_test::counted_new(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return playback_test::counted_new(size);
}
void* operator new(std::size_t size, std::align_val_t a) {
    if (void* p = playback_test::counted_new_aligned(size, a)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t a) { return operator new(size, a); }
void* operator new(std::size_t size, std::align_val_t a, const std::nothrow_t&) noexcept {
    return playback_test::counted_new_aligned(size, a);
}
void* operator new[](std::size_t size, std::align_val_t a, const std::nothrow_t&) noexcept {
    return playback_test::counted_new_aligned(size, a);
}
void operator delete(void* p) noexcept { playback_test::counted_delete(p); }
void operator delete[](void* p) noexcept { playback_test::counted_delete(p); }
void operator delete(void* p, std::size_t) noexcept { playback_test::counted_delete(p); }
void operator delete[](void* p, std::size_t) noexcept { playback_test::counted_delete(p); }
void operator delete(void* p, std::align_val_t) noexcept { playback_test::counted_delete(p); }
void operator delete[](void* p, std::align_val_t) noexcept { playback_test::counted_delete(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    playback_test::counted_delete(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    playback_test::counted_delete(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept { playback_test::counted_delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    playback_test::counted_delete(p);
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    playback_test::counted_delete(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    playback_test::counted_delete(p);
}
#endif  // PLAYBACK_TEST_COUNT_HEAP

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace playback_test {

// Source frame f of channel c. Exact in float, distinct per frame and
// channel, so a wrong chunk or a crossed channel is a wrong value.
inline float source_value(uint32_t source, uint32_t c, int64_t f) {
    return static_cast<float>(source * 1000 + c * 100) + static_cast<float>(f % 65536) / 65536.0f;
}

// A 32-bit float file, so what is read back is exactly what was written.
inline bool write_source(const std::string& path, uint32_t source, uint16_t channels,
                         int64_t frames, uint64_t time_reference = 0) {
    std::remove(path.c_str());
    sonora::WavWriter w;
    if (!w.open(path, 48000, channels, sonora::WavWriter::Format::Float32, time_reference)) {
        return false;
    }
    std::vector<float> buf(static_cast<std::size_t>(frames) * channels);
    for (int64_t f = 0; f < frames; ++f) {
        for (uint16_t c = 0; c < channels; ++c) {
            buf[static_cast<std::size_t>(f) * channels + c] = source_value(source, c, f);
        }
    }
    return w.write(buf.data(), static_cast<std::size_t>(frames)) && w.close();
}

struct SegmentSpec {
    uint32_t track = 0;            // index into the plan's tracks
    int64_t start = 0;
    int64_t end = 0;
    int64_t source_offset = 0;
    uint32_t source_slot = 0;
    uint16_t channels = 1;
    uint16_t source_channel0 = 0;  // channel c plays source_channel0 + c
    float gain = 1.0f;
    int64_t fade_in = 0;
    int64_t fade_out = 0;
    sonora::FadeCurve fade_in_shape = sonora::FadeCurve::Linear;
    sonora::FadeCurve fade_out_shape = sonora::FadeCurve::Linear;
};

// The Phase 2 graph over `tracks` engine tracks (0..tracks-1), with the
// given segments, which must be sorted by track then start. Chunk slots are
// laid out the way engine/control's builder lays them out. `track_mix`
// entries are made for the caller to fill, with mix_serial left at 0.
inline sonora::PlaybackPlan* make_plan(uint64_t epoch, uint32_t tracks,
                                       const std::vector<SegmentSpec>& specs,
                                       uint32_t chunk_frames, uint32_t edge_delay = 0,
                                       uint32_t track_mix = 0, uint32_t auto_points = 0) {
    uint32_t slots = 0;
    for (const SegmentSpec& s : specs) {
        const int64_t first = s.source_offset / chunk_frames;
        const int64_t last = (s.source_offset + (s.end - s.start) - 1) / chunk_frames;
        slots += static_cast<uint32_t>(last - first + 1) * s.channels;
    }
    sonora::PlanSizes sizes;
    sizes.tracks = tracks;
    sizes.segments = static_cast<uint32_t>(specs.size());
    sizes.nodes = tracks + 2;
    sizes.edges = tracks + 1;
    sizes.chunk_slots = slots;
    sizes.track_mix = track_mix;
    sizes.auto_points = auto_points;
    sonora::PlaybackPlan* p = sonora::create_plan(sizes);
    p->epoch = epoch;
    p->chunk_frames = chunk_frames;
    p->pan_exponent = 0.5f;
    uint32_t slot = 0;
    for (uint32_t i = 0; i < specs.size(); ++i) {
        const SegmentSpec& s = specs[i];
        sonora::PlanSegment& g = p->segments[i];
        g.start = s.start;
        g.end = s.end;
        g.source_offset = s.source_offset;
        g.channels = s.channels;
        g.gain = s.gain;
        g.fade_in_end = s.start + s.fade_in;
        g.fade_out_start = s.end - s.fade_out;
        g.fade_in_shape = s.fade_in_shape;
        g.fade_out_shape = s.fade_out_shape;
        const int64_t first = s.source_offset / chunk_frames;
        const int64_t last = (s.source_offset + (s.end - s.start) - 1) / chunk_frames;
        for (uint16_t c = 0; c < s.channels; ++c) {
            g.source_slot[c] = s.source_slot;
            g.source_channel[c] = static_cast<uint16_t>(s.source_channel0 + c);
            g.first_chunk_slot[c] = slot;
            g.first_chunk_index[c] = static_cast<uint32_t>(first);
            g.chunk_count[c] = static_cast<uint32_t>(last - first + 1);
            slot += g.chunk_count[c];
        }
    }
    for (uint32_t t = 0; t < tracks; ++t) {
        p->tracks[t].engine_track = t;
        p->tracks[t].channels = 1;
        p->tracks[t].first_segment = 0;
        p->tracks[t].segment_count = 0;
    }
    for (uint32_t i = 0; i < specs.size(); ++i) {
        sonora::PlanTrack& t = p->tracks[specs[i].track];
        if (t.segment_count == 0) t.first_segment = i;
        ++t.segment_count;
        t.channels = specs[i].channels;
    }
    sonora::layout_monitor_graph(*p);
    for (uint32_t t = 0; t < tracks; ++t) p->edges[t].delay_frames = edge_delay;
    return p;
}

// A plan with no regions over `session`'s tracks, each `track_channels`
// wide, the chains as given, laid out and compensated as engine/control's
// builder does it, against the session's own pool and maximum.
inline sonora::PlaybackPlan* chain_plan(sonora::Session& session,
                                        const std::vector<sonora::ChainBinding>& chains,
                                        sonora::DelayLinePool& pool,
                                        sonora::Compensation* comp = nullptr,
                                        std::vector<uint32_t>* shortfall = nullptr,
                                        uint16_t track_channels = 1) {
    const auto tracks = static_cast<uint32_t>(session.tracks().size());
    uint32_t active = 0;
    for (const sonora::ChainBinding& c : chains) active += c.active ? 1 : 0;
    sonora::PlanSizes sizes;
    sizes.tracks = tracks;
    sizes.nodes = sonora::graph_nodes(tracks, active);
    sizes.edges = sonora::graph_edges(tracks, active);
    sonora::PlaybackPlan* p = sonora::create_plan(sizes);
    p->epoch = session.plans().next_epoch();
    p->chunk_frames = 256;
    p->pan_exponent = 0.5f;
    for (uint32_t t = 0; t < tracks; ++t) {
        p->tracks[t].engine_track = t;
        p->tracks[t].channels = track_channels;
    }
    sonora::layout_track_graph(*p, chains.data());
    if (shortfall) shortfall->assign(tracks, 0);
    const sonora::Compensation c = sonora::compensate(*p, session.max_delay_frames(), pool,
                                                      shortfall ? shortfall->data() : nullptr);
    if (comp) *comp = c;
    return p;
}

// Makes `p` current as the control thread does: the disk reader has it
// resident before the session may see it.
inline bool publish_plan(sonora::Session& session, sonora::PlaybackPlan* p) {
    sonora::DiskReader& r = session.reader();
    const uint64_t gen = r.serve(session.plans().current(), p);
    r.run_pass();
    if (r.resident() != gen) return false;
    if (!session.plans().publish(p)) return false;
    r.serve(p, nullptr);
    r.run_pass();
    return true;
}

}  // namespace playback_test
