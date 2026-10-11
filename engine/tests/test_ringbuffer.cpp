// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/ringbuffer.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

int main() {
    // --- capacity rounds up to a power of two ---
    {
        sonora::RingBuffer<float> rb(1000);
        CHECK(rb.capacity() == 1024);
    }

    // --- write then read returns the same data ---
    {
        sonora::RingBuffer<float> rb(64);
        std::vector<float> in{1.0f, 2.0f, 3.0f, 4.0f};
        CHECK(rb.write(in.data(), in.size()) == 4);
        CHECK(rb.read_space() == 4);
        std::vector<float> out(4, 0.0f);
        CHECK(rb.read(out.data(), 4) == 4);
        CHECK(out == in);
        CHECK(rb.read_space() == 0);
    }

    // --- a short write is reported, not silently dropped ---
    {
        sonora::RingBuffer<float> rb(8);  // capacity 8, usable 7
        std::vector<float> in(16, 1.0f);
        CHECK(rb.write(in.data(), in.size()) == 7);
    }

    // --- wrapping across the end of the buffer preserves order ---
    {
        sonora::RingBuffer<int> rb(8);
        std::vector<int> a{1, 2, 3, 4, 5};
        rb.write(a.data(), a.size());
        std::vector<int> drain(5);
        rb.read(drain.data(), 5);
        std::vector<int> b{6, 7, 8, 9, 10};
        rb.write(b.data(), b.size());  // now straddles the wrap point
        std::vector<int> out(5);
        CHECK(rb.read(out.data(), 5) == 5);
        CHECK(out == b);
    }

    // --- concurrent producer/consumer loses nothing and reorders nothing ---
    {
        sonora::RingBuffer<int> rb(1024);
        constexpr int kTotal = 200000;

        std::thread producer([&] {
            int next = 0;
            while (next < kTotal) {
                int batch[64];
                int n = 0;
                while (n < 64 && next + n < kTotal) {
                    batch[n] = next + n;
                    ++n;
                }
                next += static_cast<int>(rb.write(batch, n));
            }
        });

        int expect = 0;
        int buf[128];
        while (expect < kTotal) {
            const std::size_t got = rb.read(buf, 128);
            for (std::size_t i = 0; i < got; ++i) {
                if (buf[i] != expect) {
                    std::fprintf(stderr, "FAIL sequence: got %d want %d\n",
                                 buf[i], expect);
                    producer.join();
                    return 1;
                }
                ++expect;
            }
        }
        producer.join();
        CHECK(expect == kTotal);
    }

    std::printf("test_ringbuffer: ok\n");
    return 0;
}
