// SPDX-License-Identifier: GPL-3.0-or-later
// FLAC's bit-level pieces: the bit writer, the frame checksums and the
// subframe encoder. Private to the library (not installed).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sonora {

class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}
    void put(uint64_t v, int n) {   // n <= 32
        if (n <= 0) return;
        acc_ = (acc_ << n) | (v & ((uint64_t{1} << n) - 1));
        nbits_ += n;
        while (nbits_ >= 8) {
            out_.push_back(static_cast<uint8_t>(acc_ >> (nbits_ - 8)));
            nbits_ -= 8;
        }
        acc_ &= (uint64_t{1} << nbits_) - 1;
    }
    void unary_zeroes_then_one(uint64_t q) {
        while (q > 0) {
            const int n = static_cast<int>(std::min<uint64_t>(q, 24));
            put(0, n);
            q -= static_cast<uint64_t>(n);
        }
        put(1, 1);
    }
    void align() {
        if (nbits_ > 0) put(0, 8 - nbits_);
    }

private:
    std::vector<uint8_t>& out_;
    uint64_t acc_ = 0;
    int nbits_ = 0;
};

uint8_t crc8(const uint8_t* p, std::size_t n);
uint16_t crc16(const uint8_t* p, std::size_t n);
void put_utf8_number(std::vector<uint8_t>& b, uint64_t v);

// One channel's block as a FLAC subframe: constant, fixed predictor of order
// 0 to 4 with a partitioned Rice residual, or verbatim, whichever is shortest.
void encode_subframe(BitWriter& bw, const int32_t* x, std::size_t n, int bps);

}  // namespace sonora
