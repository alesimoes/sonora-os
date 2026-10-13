// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/midi_buffer.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

// Every allocation in this executable goes through these, so a test can
// prove that a stretch of code allocated nothing at all.
namespace {
std::atomic<long> g_allocations{0};
}  // namespace

void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size > 0 ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

int main() {
    // --- messages come out in sample-offset order, whatever the push order ---
    {
        sonora::MidiBuffer buf(16, 256);
        CHECK(buf.push(64, 0x90, 60, 100));
        CHECK(buf.push(0, 0x90, 62, 100));
        CHECK(buf.push(32, 0x80, 60, 0));
        CHECK(buf.size() == 3);
        CHECK(buf[0].offset == 0 && buf[0].data[1] == 62);
        CHECK(buf[1].offset == 32 && buf[1].data[0] == 0x80);
        CHECK(buf[2].offset == 64 && buf[2].data[1] == 60);
    }

    // --- same offset keeps push order: a CC must stay ahead of its note ---
    {
        sonora::MidiBuffer buf(16, 256);
        CHECK(buf.push(10, 0xB0, 7, 90));
        CHECK(buf.push(10, 0x90, 60, 100));
        CHECK(buf.push(5, 0xC0, 3, 0));
        CHECK(buf.push(10, 0x80, 60, 0));
        CHECK(buf[0].data[0] == 0xC0 && buf[0].size == 2);
        CHECK(buf[1].data[0] == 0xB0);
        CHECK(buf[2].data[0] == 0x90);
        CHECK(buf[3].data[0] == 0x80);
    }

    // --- message length follows the status byte ---
    {
        CHECK(sonora::MidiBuffer::short_message_size(0x92) == 3);
        CHECK(sonora::MidiBuffer::short_message_size(0xC5) == 2);
        CHECK(sonora::MidiBuffer::short_message_size(0xD0) == 2);
        CHECK(sonora::MidiBuffer::short_message_size(0xE1) == 3);
        CHECK(sonora::MidiBuffer::short_message_size(0xF2) == 3);
        CHECK(sonora::MidiBuffer::short_message_size(0xF3) == 2);
        CHECK(sonora::MidiBuffer::short_message_size(0xF8) == 1);
    }

    // --- SysEx is carried whole, bytes intact ---
    {
        sonora::MidiBuffer buf(4, 64);
        const std::uint8_t sysex[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
        CHECK(buf.push(3, sysex, sizeof sysex));
        CHECK(buf.size() == 1 && buf[0].size == sizeof sysex);
        for (std::size_t i = 0; i < sizeof sysex; ++i) {
            CHECK(buf[0].data[i] == sysex[i]);
        }
    }

    // --- full on messages or on bytes: dropped and counted, never grown ---
    {
        sonora::MidiBuffer buf(2, 64);
        CHECK(buf.push(0, 0x90, 60, 1));
        CHECK(buf.push(1, 0x90, 61, 1));
        CHECK(!buf.push(2, 0x90, 62, 1));
        CHECK(buf.size() == 2 && buf.dropped() == 1);

        sonora::MidiBuffer small(8, 5);
        CHECK(small.push(0, 0x90, 60, 1));
        CHECK(!small.push(0, 0x90, 61, 1));  // 3 + 3 > 5
        CHECK(small.push(0, 0xC0, 1, 0));    // 3 + 2 == 5 still fits
        CHECK(small.dropped() == 1);

        const std::uint8_t empty = 0;
        CHECK(!small.push(0, &empty, 0));
        CHECK(small.dropped() == 2);
    }

    // --- clear() empties the block and keeps the drop count ---
    {
        sonora::MidiBuffer buf(1, 8);
        CHECK(buf.push(0, 0x90, 60, 1));
        CHECK(!buf.push(0, 0x90, 60, 1));
        buf.clear();
        CHECK(buf.empty() && buf.dropped() == 1);
        CHECK(buf.push(7, 0x80, 60, 0));
        CHECK(buf[0].offset == 7);
    }

    // --- nothing after construction allocates: the audio-thread contract ---
    {
        sonora::MidiBuffer buf(64, 1024);
        const std::uint8_t sysex[] = {0xF0, 0x43, 0x10, 0x4C, 0xF7};
        const long before = g_allocations.load();
        for (int block = 0; block < 1000; ++block) {
            buf.clear();
            // Reverse order forces the shifting path on every push.
            for (std::uint32_t i = 0; i < 80; ++i) {
                buf.push(127 - i, 0x90, static_cast<std::uint8_t>(i), 100);
            }
            buf.push(5, sysex, sizeof sysex);
            std::uint32_t last = 0;
            for (std::size_t i = 0; i < buf.size(); ++i) {
                const sonora::MidiBuffer::Message m = buf[i];
                CHECK(m.offset >= last);
                last = m.offset;
            }
        }
        CHECK(g_allocations.load() == before);
        CHECK(buf.dropped() > 0);

        // The counter is live: the same check would see an allocation. The
        // volatile stops the compiler eliding the new/delete pair.
        static int* volatile probe = nullptr;
        probe = new int(1);
        CHECK(g_allocations.load() == before + 1);
        delete probe;
    }

    std::printf("midi_buffer: all tests passed\n");
    return 0;
}
