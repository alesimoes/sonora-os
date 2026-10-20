// SPDX-License-Identifier: GPL-3.0-or-later
//
// The worker's ring without a plugin: messages wrap intact, a full ring
// refuses rather than overwriting, a drain stops at its mark, and one
// producer and one consumer on two threads lose and reorder nothing.

#include "lv2_worker.h"

#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using sonora::plugin_host::WorkerRing;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

}  // namespace

int main() {
    std::printf("worker ring\n");
    {
        WorkerRing r(64);
        std::vector<unsigned char> out(r.max_message());
        uint32_t size = 0;
        check(r.max_message() == 60, "a 64-byte ring carries messages of up to 60 bytes");
        check(!r.push(out.data(), 61), "a message larger than the ring is refused");
        // 4 + 20 bytes each: the third wraps past the end.
        bool intact = true;
        for (uint32_t round = 0; round < 10; ++round) {
            unsigned char msg[20];
            for (int i = 0; i < 20; ++i) msg[i] = static_cast<unsigned char>(round * 20 + i);
            if (!r.push(msg, sizeof msg)) intact = false;
            if (!r.pop(r.mark(), out.data(), size) || size != 20 || std::memcmp(msg, out.data(), 20) != 0)
                intact = false;
        }
        check(intact, "messages that wrap around the end come out intact");
        unsigned char a[24] = {}, b[24] = {}, c[24] = {};
        check(r.push(a, sizeof a) && r.push(b, sizeof b), "two 28-byte messages fit in 64 bytes");
        check(!r.push(c, sizeof c), "a third is refused, not written over the first");
        const uint32_t until = r.mark();
        check(r.pop(until, out.data(), size) && size == 24, "the first comes out");
        check(r.push(c, sizeof c), "and makes room for the third");
        check(r.pop(until, out.data(), size) && size == 24, "a drain to its mark takes the second");
        check(!r.pop(until, out.data(), size), "and stops there, though the third is queued");
        check(r.pop(r.mark(), out.data(), size) && size == 24, "a later drain takes it");
        check(r.push(nullptr, 0) && r.pop(r.mark(), out.data(), size) && size == 0, "an empty message passes");
    }
    {
        WorkerRing r(256);
        constexpr uint32_t kCount = 200000;
        std::thread producer([&] {
            for (uint32_t i = 0; i < kCount;) {
                const uint32_t len = 1 + i % 37;
                unsigned char msg[64];
                for (uint32_t k = 0; k < len; ++k) msg[k] = static_cast<unsigned char>(i + k);
                if (r.push(msg, len)) ++i;
                else std::this_thread::yield();
            }
        });
        std::vector<unsigned char> out(r.max_message());
        bool ordered = true;
        for (uint32_t i = 0; i < kCount;) {
            uint32_t size = 0;
            if (!r.pop(r.mark(), out.data(), size)) {
                std::this_thread::yield();
                continue;
            }
            const uint32_t len = 1 + i % 37;
            if (size != len) ordered = false;
            for (uint32_t k = 0; k < size && k < len; ++k)
                if (out[k] != static_cast<unsigned char>(i + k)) ordered = false;
            ++i;
        }
        producer.join();
        check(ordered, "200000 messages across two threads arrive whole and in order");
    }
    return failures == 0 ? 0 : 1;
}
