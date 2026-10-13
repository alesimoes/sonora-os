// SPDX-License-Identifier: GPL-3.0-or-later
#include "urid_map.h"

#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/midi/midi.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/patch/patch.h>
#include <lv2/time/time.h>

#include <cstring>

namespace sonora::plugin_host {

namespace {

uint32_t fnv1a(const char* s, std::size_t& len) noexcept {
    uint32_t h = 2166136261u;
    const char* p = s;
    for (; *p; ++p) h = (h ^ static_cast<unsigned char>(*p)) * 16777619u;
    len = static_cast<std::size_t>(p - s);
    return h;
}

// Mapped up front so plugins asking for them from run() only read the table.
const char* const kKnown[] = {
    LV2_ATOM__Atom, LV2_ATOM__Blank, LV2_ATOM__Bool, LV2_ATOM__Chunk, LV2_ATOM__Double,
    LV2_ATOM__Event, LV2_ATOM__Float, LV2_ATOM__Int, LV2_ATOM__Literal, LV2_ATOM__Long,
    LV2_ATOM__Number, LV2_ATOM__Object, LV2_ATOM__Path, LV2_ATOM__Property, LV2_ATOM__Resource,
    LV2_ATOM__Sequence, LV2_ATOM__Sound, LV2_ATOM__String, LV2_ATOM__Tuple, LV2_ATOM__URI,
    LV2_ATOM__URID, LV2_ATOM__Vector, LV2_ATOM__atomTransfer, LV2_ATOM__beatTime,
    LV2_ATOM__eventTransfer, LV2_ATOM__frameTime, LV2_MIDI__MidiEvent,
    LV2_TIME__Position, LV2_TIME__bar, LV2_TIME__barBeat, LV2_TIME__beat, LV2_TIME__beatUnit,
    LV2_TIME__beatsPerBar, LV2_TIME__beatsPerMinute, LV2_TIME__frame, LV2_TIME__framesPerSecond,
    LV2_TIME__speed, LV2_PATCH__Get, LV2_PATCH__Set, LV2_PATCH__Put, LV2_PATCH__property,
    LV2_PATCH__value, LV2_PATCH__subject, LV2_PATCH__body, LV2_BUF_SIZE__minBlockLength,
    LV2_BUF_SIZE__maxBlockLength, LV2_BUF_SIZE__nominalBlockLength,
    LV2_BUF_SIZE__sequenceSize, LV2_PARAMETERS__sampleRate, LV2_OPTIONS__interface,
};

}  // namespace

UridMap::UridMap(std::size_t slots, std::size_t arena_bytes) {
    std::size_t n = 1;
    while (n < slots) n <<= 1;
    mask_ = n - 1;
    // Linear probing stays short below three quarters full.
    max_count_ = n / 4 * 3;
    slots_.reset(new std::atomic<uint64_t>[n]);
    for (std::size_t i = 0; i < n; ++i) slots_[i].store(0, std::memory_order_relaxed);
    // The offset is stored in 32 bits.
    arena_bytes_ = arena_bytes < 0xFFFFFFFFu ? arena_bytes : 0xFFFFFFFEu;
    arena_.reset(new char[arena_bytes_]);

    map_.handle = this;
    map_.map = [](LV2_URID_Map_Handle h, const char* uri) { return static_cast<UridMap*>(h)->map(uri); };
    unmap_.handle = this;
    unmap_.unmap = [](LV2_URID_Unmap_Handle h, LV2_URID id) {
        return static_cast<UridMap*>(h)->unmap(id);
    };
    for (const char* uri : kKnown) map(uri);
}

LV2_URID UridMap::refuse() noexcept {
    refused_.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

LV2_URID UridMap::find(const char* uri) const noexcept {
    if (!uri) return 0;
    std::size_t len;
    const uint32_t h = fnv1a(uri, len);
    for (std::size_t probe = 0; probe <= mask_; ++probe) {
        const std::size_t i = (h + probe) & mask_;
        const uint64_t k = slots_[i].load(std::memory_order_acquire);
        if (k == 0) return 0;  // slots never empty again: the chain ends here
        if (uint32_t(k >> 32) == h &&
            std::strcmp(arena_.get() + (uint32_t(k) - 1), uri) == 0)
            return static_cast<LV2_URID>(i + 1);
    }
    return 0;
}

LV2_URID UridMap::map(const char* uri) noexcept {
    if (!uri) return 0;
    if (const LV2_URID id = find(uri)) return id;
    if (count_.load(std::memory_order_relaxed) >= max_count_) return refuse();

    // Copy the text first, then publish it with one CAS. A thread that loses
    // a race for the same URI leaves its copy unused in the arena: a few
    // bytes, against a lock or a slot half-written.
    std::size_t len;
    const uint32_t h = fnv1a(uri, len);
    const std::size_t off = arena_used_.fetch_add(len + 1, std::memory_order_relaxed);
    if (off + len + 1 > arena_bytes_) return refuse();
    std::memcpy(arena_.get() + off, uri, len + 1);
    const uint64_t key = (uint64_t(h) << 32) | uint64_t(off + 1);

    for (std::size_t probe = 0; probe <= mask_; ++probe) {
        const std::size_t i = (h + probe) & mask_;
        uint64_t k = slots_[i].load(std::memory_order_acquire);
        if (k == 0) {
            if (slots_[i].compare_exchange_strong(k, key, std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                count_.fetch_add(1, std::memory_order_relaxed);
                return static_cast<LV2_URID>(i + 1);
            }
            // Lost the slot; k now holds the winner, which may be this URI.
        }
        if (uint32_t(k >> 32) == h &&
            std::strcmp(arena_.get() + (uint32_t(k) - 1), uri) == 0)
            return static_cast<LV2_URID>(i + 1);
    }
    return refuse();
}

const char* UridMap::unmap(LV2_URID id) const noexcept {
    if (id == 0 || id > mask_ + 1) return nullptr;
    const uint64_t k = slots_[id - 1].load(std::memory_order_acquire);
    return k ? arena_.get() + (uint32_t(k) - 1) : nullptr;
}

}  // namespace sonora::plugin_host
