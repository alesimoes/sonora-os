// SPDX-License-Identifier: GPL-3.0-or-later
//
// The heap counter in playback_test.h is what proves Session::process()
// allocates nothing, so it must build wherever the tests build and must see
// every allocator it replaces. Its overrides come before the system headers
// that declare them again: <malloc.h> declares malloc, calloc, realloc,
// memalign, free; <random> reaches posix_memalign through <mm_malloc.h>
// when SSE3 is on (Ubuntu 26.04's x86-64-v3 GCC). Building this file is half
// the test: an override whose exception specifier differs from glibc's is a
// hard error here.
#define PLAYBACK_TEST_COUNT_HEAP 1
#include "playback_test.h"

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include <cstdlib>
#include <random>

namespace {

using namespace playback_test;

// volatile so the compiler cannot drop a malloc/free pair it can see through.
void* volatile g_sink = nullptr;

int test_counts_each_allocator() {
#if defined(PLAYBACK_TEST_COUNTS_MALLOC)
    reset_heap_counts();
    t_counting = true;
    g_sink = std::malloc(16);
    std::free(g_sink);
    g_sink = std::calloc(2, 8);
    g_sink = std::realloc(g_sink, 64);
    std::free(g_sink);
    g_sink = std::aligned_alloc(64, 64);
    std::free(g_sink);
    g_sink = memalign(64, 64);
    std::free(g_sink);
    void* p = nullptr;
    const int rc = posix_memalign(&p, 64, 64);
    g_sink = p;
    std::free(g_sink);
    t_counting = false;
    CHECK(rc == 0);
    CHECK(g_allocations.load() == 6);
    CHECK(g_frees.load() == 5);
#endif
    reset_heap_counts();
    t_counting = true;
    delete new int(1);
    t_counting = false;
    CHECK(!heap_untouched());
    return 0;
}

}  // namespace

int main() {
    if (test_counts_each_allocator() != 0) return 1;
    std::printf("heap counter: %s\n", heap_counter_scope());
    return 0;
}
