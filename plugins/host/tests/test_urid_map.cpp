// SPDX-License-Identifier: GPL-3.0-or-later
//
// The URID map plugins call from run() on the host's SCHED_FIFO thread:
// referentially transparent, unmap pointers that never move, and no lock or
// allocation once constructed. Regressions: the first version took a
// std::mutex and grew a std::vector<std::string>, whose growth moved short
// (SSO) strings and left earlier unmap() pointers dangling.
#include "urid_map.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>

using sonora::plugin_host::UridMap;

// Counts every allocation in this process while `g_counting` is set.
static std::atomic<bool> g_counting{false};
static std::atomic<unsigned long> g_allocations{0};

void* operator new(std::size_t n) {
    if (g_counting.load(std::memory_order_relaxed)) g_allocations.fetch_add(1);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

int main() {
    UridMap m;
    LV2_URID_Map* map = m.map_feature();
    LV2_URID_Unmap* unmap = m.unmap_feature();

    // Known URIs are mapped at construction.
    CHECK(m.size() >= 30);
    CHECK(m.find("http://lv2plug.in/ns/ext/atom#Sequence") != 0);
    CHECK(m.find("http://lv2plug.in/ns/ext/midi#MidiEvent") != 0);
    CHECK(m.find("urn:not-mapped-yet") == 0);

    // Short URIs, which live inside std::string's SSO buffer: the old
    // vector<string> moved them on growth.
    const LV2_URID a = map->map(map->handle, "urn:a");
    const LV2_URID b = map->map(map->handle, "urn:b");
    CHECK(a != 0 && b != 0 && a != b);
    CHECK(map->map(map->handle, "urn:a") == a);
    const char* a_str = unmap->unmap(unmap->handle, a);
    CHECK(a_str && std::strcmp(a_str, "urn:a") == 0);

    // Map and unmap after construction: no allocation at all, whether the
    // URI is known, pre-mapped or new.
    std::vector<std::string> uris;
    for (int i = 0; i < 2000; ++i) uris.push_back("urn:sonora:test:uri:" + std::to_string(i));
    std::vector<LV2_URID> ids(uris.size());
    g_allocations = 0;
    g_counting = true;
    for (std::size_t i = 0; i < uris.size(); ++i) ids[i] = map->map(map->handle, uris[i].c_str());
    const LV2_URID seq = map->map(map->handle, "http://lv2plug.in/ns/ext/atom#Sequence");
    const char* seq_str = unmap->unmap(unmap->handle, seq);
    g_counting = false;
    CHECK(g_allocations == 0);
    CHECK(seq_str && std::strcmp(seq_str, "http://lv2plug.in/ns/ext/atom#Sequence") == 0);

    // The early pointer did not move or change after 2000 more insertions.
    CHECK(unmap->unmap(unmap->handle, a) == a_str);
    CHECK(std::strcmp(a_str, "urn:a") == 0);
    for (std::size_t i = 0; i < uris.size(); ++i) {
        CHECK(ids[i] != 0);
        CHECK(map->map(map->handle, uris[i].c_str()) == ids[i]);
        const char* s = unmap->unmap(unmap->handle, ids[i]);
        CHECK(s && uris[i] == s);
    }
    CHECK(unmap->unmap(unmap->handle, 0) == nullptr);
    CHECK(unmap->unmap(unmap->handle, 0xFFFFFFFFu) == nullptr);

    // Two threads mapping the same new URIs at once agree on every ID.
    std::vector<std::string> shared;
    for (int i = 0; i < 500; ++i) shared.push_back("urn:sonora:race:" + std::to_string(i));
    std::vector<LV2_URID> t1(shared.size()), t2(shared.size());
    std::thread th([&] {
        for (std::size_t i = 0; i < shared.size(); ++i) t1[i] = map->map(map->handle, shared[i].c_str());
    });
    for (std::size_t i = shared.size(); i-- > 0;) t2[i] = map->map(map->handle, shared[i].c_str());
    th.join();
    for (std::size_t i = 0; i < shared.size(); ++i) CHECK(t1[i] != 0 && t1[i] == t2[i]);

    // Full: further new URIs map to 0 (LV2 allows it: "could not be
    // mapped"), are counted, and nothing already mapped changes.
    UridMap small(64, 2048);
    for (int i = 0; i < 200; ++i) small.map(("urn:fill:" + std::to_string(i)).c_str());
    CHECK(small.refused() > 0);
    CHECK(small.map("urn:fill:never-fits-now") == 0);
    CHECK(small.size() <= 64);
    const LV2_URID first = small.find("http://lv2plug.in/ns/ext/atom#Int");
    CHECK(first != 0 && small.map("http://lv2plug.in/ns/ext/atom#Int") == first);

    if (failures) return EXIT_FAILURE;
    std::printf("urid map: ok (%zu URIs mapped, no allocation after construction)\n", m.size());
    return EXIT_SUCCESS;
}
