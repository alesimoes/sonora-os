// SPDX-License-Identifier: GPL-3.0-or-later
// Test doubles for the LTC sample boundary (ADR-0124). Every buffer is
// sized at construction, so a test can check the drivers stay
// allocation-free behind them.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "sonora/ltc/ltc_sync.h"

namespace sonora::ltc {

// Hands out a fixed recording, at most `chunk` samples per read, as a tap
// delivering one period at a time would.
class FakeLtcSource final : public LtcSampleSource {
public:
    FakeLtcSource(std::vector<float> samples, std::size_t chunk)
        : samples_(std::move(samples)), chunk_(chunk) {}

    std::size_t read(Span<float> out) override {
        ++reads_;
        const std::size_t n = std::min({out.size, chunk_, samples_.size() - pos_});
        std::copy_n(samples_.data() + pos_, n, out.data);
        pos_ += n;
        return n;
    }

    std::size_t remaining() const { return samples_.size() - pos_; }
    std::uint64_t reads() const { return reads_; }
    void rewind() { pos_ = 0; }

private:
    std::vector<float> samples_;
    std::size_t chunk_;
    std::size_t pos_ = 0;
    std::uint64_t reads_ = 0;
};

// Keeps up to `capacity` samples; past that a write is short, as a full
// ring would be.
class FakeLtcSink final : public LtcSampleSink {
public:
    explicit FakeLtcSink(std::size_t capacity) : capacity_(capacity) {
        samples_.reserve(capacity);
    }

    std::size_t write(Span<const float> in) override {
        const std::size_t n = std::min(in.size, capacity_ - samples_.size());
        samples_.insert(samples_.end(), in.data, in.data + n);
        if (n < in.size) ++short_writes_;
        return n;
    }

    const std::vector<float>& samples() const { return samples_; }
    std::uint64_t short_writes() const { return short_writes_; }
    // Empties the sink and keeps its storage.
    void clear() { samples_.clear(); }

private:
    std::size_t capacity_;
    std::vector<float> samples_;
    std::uint64_t short_writes_ = 0;
};

}  // namespace sonora::ltc
