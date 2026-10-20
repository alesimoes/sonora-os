// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0015 Unverified 6: sha256_hex's throughput on 100 MiB, about what the
// MIDI of a 16 M-entry session encodes to. Run by hand; the numbers go in
// the PR. Prints the best of three, so a page fault on the first pass or a
// noisy neighbour does not set the figure.
#include "sonora/sha256.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    const std::size_t mib = argc > 1 ? static_cast<std::size_t>(std::stoul(argv[1])) : 100;
    std::string bytes(mib << 20, '\0');
    // Not all zeros: a xorshift stream, so nothing about the input is special.
    std::uint64_t x = 0x9E3779B97F4A7C15ull;
    for (char& c : bytes) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        c = static_cast<char>(x);
    }
    double best = 1e30;
    std::string digest;
    for (int pass = 0; pass < 3; ++pass) {
        const auto t0 = std::chrono::steady_clock::now();
        digest = sonora::sha256_hex(bytes);
        const double s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (s < best) best = s;
    }
    std::printf("sha256_hex: %zu MiB in %.3f s, %.1f MiB/s (best of 3; %s)\n", mib, best,
                static_cast<double>(mib) / best, digest.substr(0, 12).c_str());
    return 0;
}
