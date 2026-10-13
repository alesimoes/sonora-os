// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lv2/urid/urid.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace sonora::plugin_host {

// The URID map shared by every instance in this process. Plugins call it from
// instantiate() and, in practice, from run() on the SCHED_FIFO processing
// thread, and sometimes from threads of their own at the same time. So after
// construction neither map() nor unmap() locks or allocates:
//
//  - The table is open-addressed with a fixed number of slots, each one
//    atomic word: the URI's hash and the offset of its text in the arena.
//    A slot goes from empty to filled once, by compare-and-swap, and never
//    changes again, so a lookup is a few acquire loads and a strcmp.
//  - URI text lives in a fixed arena, appended with fetch_add and never
//    moved, so an unmap() pointer stays valid for the life of the map.
//  - The ID is the slot index + 1: stable, and unmap needs no second table.
//
// When the table or the arena is full, a new URI maps to 0, which LV2 allows
// ("could not be mapped"), and refused() counts it. Both limits only fill up,
// so a URI refused once is refused for good: the map stays referentially
// transparent. The URIs this host and common plugins use are mapped in the
// constructor, so a plugin's run() normally finds what it asks for.
class UridMap {
public:
    explicit UridMap(std::size_t slots = 8192, std::size_t arena_bytes = 512 * 1024);
    UridMap(const UridMap&) = delete;
    UridMap& operator=(const UridMap&) = delete;

    LV2_URID map(const char* uri) noexcept;
    const char* unmap(LV2_URID id) const noexcept;
    LV2_URID find(const char* uri) const noexcept;  // 0 when not mapped

    std::size_t size() const noexcept { return count_.load(std::memory_order_relaxed); }
    uint64_t refused() const noexcept { return refused_.load(std::memory_order_relaxed); }

    LV2_URID_Map* map_feature() { return &map_; }
    LV2_URID_Unmap* unmap_feature() { return &unmap_; }

private:
    LV2_URID refuse() noexcept;

    std::size_t mask_;
    std::size_t max_count_;
    std::unique_ptr<std::atomic<uint64_t>[]> slots_;  // 0, or hash << 32 | (offset + 1)
    std::size_t arena_bytes_;
    std::unique_ptr<char[]> arena_;
    std::atomic<std::size_t> arena_used_{0};
    std::atomic<std::size_t> count_{0};
    std::atomic<uint64_t> refused_{0};
    LV2_URID_Map map_;
    LV2_URID_Unmap unmap_;
};

}  // namespace sonora::plugin_host
