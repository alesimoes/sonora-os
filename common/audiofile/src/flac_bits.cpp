// SPDX-License-Identifier: GPL-3.0-or-later
#include "flac_bits.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace sonora {

uint8_t crc8(const uint8_t* p, std::size_t n) {
    uint8_t c = 0;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int b = 0; b < 8; ++b) c = static_cast<uint8_t>((c & 0x80) ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}

uint16_t crc16(const uint8_t* p, std::size_t n) {
    static uint16_t table[256];
    static bool made = false;
    static std::mutex m;
    {
        const std::lock_guard<std::mutex> lock(m);
        if (!made) {
            for (uint32_t i = 0; i < 256; ++i) {
                uint16_t c = static_cast<uint16_t>(i << 8);
                for (int b = 0; b < 8; ++b) {
                    c = static_cast<uint16_t>((c & 0x8000) ? (c << 1) ^ 0x8005 : c << 1);
                }
                table[i] = c;
            }
            made = true;
        }
    }
    uint16_t c = 0;
    for (std::size_t i = 0; i < n; ++i) {
        c = static_cast<uint16_t>((c << 8) ^ table[(c >> 8) ^ p[i]]);
    }
    return c;
}

void put_utf8_number(std::vector<uint8_t>& b, uint64_t v) {
    if (v < 0x80) {
        b.push_back(static_cast<uint8_t>(v));
        return;
    }
    int extra = 1;
    while (extra < 6 && v >= (uint64_t{1} << (5 * extra + 6))) ++extra;
    b.push_back(static_cast<uint8_t>((0xFF00u >> (extra + 1)) | (v >> (6 * extra))));
    for (int i = extra - 1; i >= 0; --i) {
        b.push_back(static_cast<uint8_t>(0x80 | ((v >> (6 * i)) & 0x3F)));
    }
}

void encode_subframe(BitWriter& bw, const int32_t* x, std::size_t n, int bps) {
    bool constant = true;
    for (std::size_t i = 1; i < n && constant; ++i) constant = x[i] == x[0];
    if (constant) {
        bw.put(0, 8);   // pad 0, type CONSTANT (000000), no wasted bits
        bw.put(static_cast<uint32_t>(x[0]), bps);
        return;
    }

    int best_order = -1;
    uint64_t best_abs = ~uint64_t{0};
    std::vector<uint64_t> u;
    if (n > 4) {
        for (int order = 0; order <= 4; ++order) {
            uint64_t sum = 0;
            for (std::size_t i = static_cast<std::size_t>(order); i < n; ++i) {
                const int64_t a = x[i], b = i >= 1 ? x[i - 1] : 0, c = i >= 2 ? x[i - 2] : 0,
                              d = i >= 3 ? x[i - 3] : 0, e = i >= 4 ? x[i - 4] : 0;
                int64_t r = 0;
                switch (order) {
                    case 0: r = a; break;
                    case 1: r = a - b; break;
                    case 2: r = a - 2 * b + c; break;
                    case 3: r = a - 3 * b + 3 * c - d; break;
                    default: r = a - 4 * b + 6 * c - 4 * d + e; break;
                }
                sum += static_cast<uint64_t>(r < 0 ? -r : r);
            }
            if (sum < best_abs) {
                best_abs = sum;
                best_order = order;
            }
        }
    }

    int best_part = 0;
    uint64_t best_bits = static_cast<uint64_t>(n) * static_cast<uint64_t>(bps);   // verbatim
    std::vector<int> best_k;
    if (best_order >= 0) {
        const std::size_t order = static_cast<std::size_t>(best_order);
        const std::size_t count = n - order;
        u.resize(count);
        for (std::size_t i = order; i < n; ++i) {
            const int64_t a = x[i], b = i >= 1 ? x[i - 1] : 0, c = i >= 2 ? x[i - 2] : 0,
                          d = i >= 3 ? x[i - 3] : 0, e = i >= 4 ? x[i - 4] : 0;
            int64_t r = 0;
            switch (best_order) {
                case 0: r = a; break;
                case 1: r = a - b; break;
                case 2: r = a - 2 * b + c; break;
                case 3: r = a - 3 * b + 3 * c - d; break;
                default: r = a - 4 * b + 6 * c - 4 * d + e; break;
            }
            u[i - order] = static_cast<uint64_t>((r << 1) ^ (r >> 63));
        }
        const uint64_t head = 8 + order * static_cast<uint64_t>(bps) + 2 + 4;
        for (int p = 0; p <= 6; ++p) {
            const std::size_t parts = std::size_t{1} << p;
            if (n % parts != 0 || (n >> p) <= order) continue;
            const std::size_t per = n >> p;
            uint64_t bits = head;
            std::vector<int> ks(parts);
            for (std::size_t q = 0; q < parts; ++q) {
                const std::size_t lo = q == 0 ? 0 : q * per - order;
                const std::size_t hi = (q + 1) * per - order;
                uint64_t total = 0;
                for (std::size_t i = lo; i < hi; ++i) total += u[i];
                const double mean = static_cast<double>(total) / static_cast<double>(hi - lo);
                // Capped first: a mean past 2^25 would leave the search empty.
                const int k0 =
                    mean < 1.0 ? 0 : std::min(24, static_cast<int>(std::floor(std::log2(mean))));
                uint64_t best_cost = ~uint64_t{0};
                int best = 0;
                for (int k = std::max(0, k0 - 1); k <= std::min(24, k0 + 1); ++k) {
                    uint64_t cost = static_cast<uint64_t>(hi - lo) * static_cast<uint64_t>(k + 1);
                    for (std::size_t i = lo; i < hi; ++i) cost += u[i] >> k;
                    if (cost < best_cost) {
                        best_cost = cost;
                        best = k;
                    }
                }
                ks[q] = best;
                bits += 5 + best_cost;
            }
            if (bits < best_bits) {
                best_bits = bits;
                best_part = p;
                best_k = ks;
            }
        }
    }

    if (best_k.empty()) {   // verbatim
        bw.put(0, 1);
        bw.put(1, 6);
        bw.put(0, 1);
        for (std::size_t i = 0; i < n; ++i) bw.put(static_cast<uint32_t>(x[i]), bps);
        return;
    }
    const std::size_t order = static_cast<std::size_t>(best_order);
    bw.put(0, 1);
    bw.put(8u | static_cast<uint32_t>(best_order), 6);   // 001xxx FIXED
    bw.put(0, 1);
    for (std::size_t i = 0; i < order; ++i) bw.put(static_cast<uint32_t>(x[i]), bps);
    bw.put(1, 2);   // Rice, 5-bit parameters: a loud 24-bit residual wants k past 14
    bw.put(static_cast<uint32_t>(best_part), 4);
    const std::size_t parts = std::size_t{1} << best_part;
    const std::size_t per = n >> best_part;
    for (std::size_t q = 0; q < parts; ++q) {
        const std::size_t lo = q == 0 ? 0 : q * per - order;
        const std::size_t hi = (q + 1) * per - order;
        const int k = best_k[q];
        bw.put(static_cast<uint32_t>(k), 5);
        for (std::size_t i = lo; i < hi; ++i) {
            bw.unary_zeroes_then_one(u[i] >> k);
            bw.put(u[i], k);
        }
    }
}

}  // namespace sonora
