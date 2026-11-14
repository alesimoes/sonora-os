// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copied from engine/core/tests/playback_test.h (PLAYBACK_TEST_COUNT_HEAP),
// allocations and frees summed into one count. Kept to one translation unit:
// it replaces the global allocators.
#include "heap_counter.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace sonora::bench {
thread_local bool t_counting = false;
namespace {
std::atomic<long> g_calls{0};
}
void note_alloc() {
    if (t_counting) g_calls.fetch_add(1, std::memory_order_relaxed);
}
void note_free(void* p) {
    if (t_counting && p != nullptr) g_calls.fetch_add(1, std::memory_order_relaxed);
}
long heap_calls() { return g_calls.load(); }
void reset_heap_calls() { g_calls.store(0); }
}  // namespace sonora::bench

// RTSan interposes malloc/free itself; a strong definition here would blind
// it, and ASan/TSan keep their own interceptors.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer) || __has_feature(realtime_sanitizer)
#define SONORA_BENCH_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define SONORA_BENCH_SANITIZED 1
#endif

#if defined(__GLIBC__) && !defined(SONORA_BENCH_SANITIZED)
#define SONORA_BENCH_COUNTS_MALLOC 1
extern "C" {
void* __libc_malloc(std::size_t);
void* __libc_calloc(std::size_t, std::size_t);
void* __libc_realloc(void*, std::size_t);
void* __libc_memalign(std::size_t, std::size_t);
void __libc_free(void*);

// noexcept, as glibc's __THROW declares them (see playback_test.h).
void* malloc(std::size_t n) noexcept {
    sonora::bench::note_alloc();
    return __libc_malloc(n);
}
void* calloc(std::size_t n, std::size_t size) noexcept {
    sonora::bench::note_alloc();
    return __libc_calloc(n, size);
}
void* realloc(void* p, std::size_t n) noexcept {
    sonora::bench::note_alloc();
    return __libc_realloc(p, n);
}
void* aligned_alloc(std::size_t align, std::size_t n) noexcept {
    sonora::bench::note_alloc();
    return __libc_memalign(align, n);
}
void* memalign(std::size_t align, std::size_t n) noexcept {
    sonora::bench::note_alloc();
    return __libc_memalign(align, n);
}
int posix_memalign(void** out, std::size_t align, std::size_t n) noexcept {
    sonora::bench::note_alloc();
    void* p = __libc_memalign(align, n);
    if (p == nullptr) return 12;  // ENOMEM
    *out = p;
    return 0;
}
void free(void* p) noexcept {
    sonora::bench::note_free(p);
    __libc_free(p);
}
}
#endif

namespace sonora::bench {
const char* heap_counter_scope() {
#if defined(SONORA_BENCH_COUNTS_MALLOC)
    return "operator new/delete and malloc/calloc/realloc/aligned/free";
#else
    return "operator new/delete only (a sanitizer or non-glibc build keeps malloc)";
#endif
}
}  // namespace sonora::bench

namespace {
void* counted_new(std::size_t size) {
    sonora::bench::note_alloc();
    return std::malloc(size > 0 ? size : 1);
}
void* counted_new_aligned(std::size_t size, std::align_val_t align) {
    sonora::bench::note_alloc();
    const std::size_t a =
        static_cast<std::size_t>(align) < sizeof(void*) ? sizeof(void*) : static_cast<std::size_t>(align);
    const std::size_t n = ((size > 0 ? size : 1) + a - 1) / a * a;
    return std::aligned_alloc(a, n);
}
void counted_delete(void* p) {
    sonora::bench::note_free(p);
    std::free(p);
}
void counted_delete_aligned(void* p) { counted_delete(p); }
}  // namespace

void* operator new(std::size_t size) {
    if (void* p = counted_new(size)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return counted_new(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return counted_new(size); }
void* operator new(std::size_t size, std::align_val_t a) {
    if (void* p = counted_new_aligned(size, a)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t a) { return operator new(size, a); }
void* operator new(std::size_t size, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_new_aligned(size, a);
}
void* operator new[](std::size_t size, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_new_aligned(size, a);
}
void operator delete(void* p) noexcept { counted_delete(p); }
void operator delete[](void* p) noexcept { counted_delete(p); }
void operator delete(void* p, std::size_t) noexcept { counted_delete(p); }
void operator delete[](void* p, std::size_t) noexcept { counted_delete(p); }
void operator delete(void* p, std::align_val_t) noexcept { counted_delete_aligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { counted_delete_aligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { counted_delete_aligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { counted_delete_aligned(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { counted_delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { counted_delete(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { counted_delete_aligned(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { counted_delete_aligned(p); }
