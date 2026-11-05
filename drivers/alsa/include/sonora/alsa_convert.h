// SPDX-License-Identifier: GPL-3.0-or-later
//
// Interleaved integer samples to and from the engine's planar float. Runs on
// the backend's thread every period: no allocation, no locks.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "sonora/alsa_device.h"
#include "sonora/rt.h"

namespace sonora::alsa {
namespace detail {

inline int32_t load(SampleFormat f, const uint8_t* p) noexcept {
    switch (f) {
        case SampleFormat::S16: {
            int16_t v;
            std::memcpy(&v, p, 2);
            return v;
        }
        case SampleFormat::S24_3: {
            const uint32_t u = static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
                               static_cast<uint32_t>(p[2]) << 16;
            return static_cast<int32_t>(u << 8) >> 8;
        }
        case SampleFormat::S24_4: {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return static_cast<int32_t>(v << 8) >> 8;
        }
        case SampleFormat::S32: {
            int32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }
    }
    return 0;
}

inline float scale_of(SampleFormat f) noexcept {
    switch (f) {
        case SampleFormat::S16: return 1.0f / 32768.0f;
        case SampleFormat::S24_3:
        case SampleFormat::S24_4: return 1.0f / 8388608.0f;
        case SampleFormat::S32: return 1.0f / 2147483648.0f;
    }
    return 0.0f;
}

inline void store(SampleFormat f, uint8_t* p, float x) noexcept {
    const double full = f == SampleFormat::S16   ? 32768.0
                        : f == SampleFormat::S32 ? 2147483648.0
                                                 : 8388608.0;
    double d = static_cast<double>(x) * full;
    d += d >= 0.0 ? 0.5 : -0.5;
    if (d > full - 1.0) d = full - 1.0;
    if (d < -full) d = -full;
    const auto v = static_cast<int32_t>(d);
    switch (f) {
        case SampleFormat::S16: {
            const auto s = static_cast<int16_t>(v);
            std::memcpy(p, &s, 2);
            break;
        }
        case SampleFormat::S24_3:
            p[0] = static_cast<uint8_t>(v);
            p[1] = static_cast<uint8_t>(v >> 8);
            p[2] = static_cast<uint8_t>(v >> 16);
            break;
        case SampleFormat::S24_4: {
            const auto u = static_cast<uint32_t>(v) & 0x00FFFFFFu;
            std::memcpy(p, &u, 4);
            break;
        }
        case SampleFormat::S32:
            std::memcpy(p, &v, 4);
            break;
    }
}

}  // namespace detail

// Channels 0..n-1 of `frames` interleaved frames of `total` channels into
// dst[0..n-1], each `frames` long. Little-endian hosts only (ALSA's _LE
// formats); x86-64 and aarch64 Linux are.
inline void deinterleave(SampleFormat f, const uint8_t* src, uint32_t total, float* const* dst,
                         uint32_t n, uint32_t frames) noexcept SONORA_RT {
    const uint32_t width = bytes_per_sample(f);
    const std::size_t stride = static_cast<std::size_t>(total) * width;
    const float scale = detail::scale_of(f);
    for (uint32_t c = 0; c < n; ++c) {
        const uint8_t* p = src + static_cast<std::size_t>(c) * width;
        float* out = dst[c];
        for (uint32_t i = 0; i < frames; ++i, p += stride) {
            out[i] = static_cast<float>(detail::load(f, p)) * scale;
        }
    }
}

// src[0..n-1] into the first n of `total` interleaved channels; the rest of
// each frame is silence.
inline void interleave(SampleFormat f, float* const* src, uint32_t n, uint8_t* dst,
                       uint32_t total, uint32_t frames) noexcept SONORA_RT {
    const uint32_t width = bytes_per_sample(f);
    const std::size_t stride = static_cast<std::size_t>(total) * width;
    for (uint32_t c = 0; c < total; ++c) {
        uint8_t* p = dst + static_cast<std::size_t>(c) * width;
        if (c < n) {
            const float* in = src[c];
            for (uint32_t i = 0; i < frames; ++i, p += stride) detail::store(f, p, in[i]);
        } else {
            for (uint32_t i = 0; i < frames; ++i, p += stride) std::memset(p, 0, width);
        }
    }
}

}  // namespace sonora::alsa
