// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/audio_export.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "export_file.h"
#include "sonora/wav_reader.h"

namespace sonora {

namespace {

constexpr int kSincZeroes = 32;              // resampler half width, in zero crossings
constexpr int kTableRes = 512;               // kernel table points per zero crossing
constexpr double kKaiserBeta = 10.0;

// --- the quantiser's noise ----------------------------------------------------

constexpr float kShapeC1 = 1.0f;    // NTF = 1 - z^-1 + 0.5 z^-2
constexpr float kShapeC2 = -0.5f;

// --- the resampler -------------------------------------------------------------

double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 50; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

// Windowed sinc, one table of the low-pass at unit cutoff; a rate cut
// stretches it by the ratio.
class Kernel {
public:
    Kernel() {
        const std::size_t n = static_cast<std::size_t>(2 * kSincZeroes * kTableRes) + 2;
        table_.assign(n, 0.0f);
        const double pi = 3.14159265358979323846;
        const double i0b = bessel_i0(kKaiserBeta);
        for (std::size_t i = 0; i < n; ++i) {
            const double u = static_cast<double>(i) / kTableRes - kSincZeroes;
            if (std::fabs(u) >= kSincZeroes) continue;
            const double s = u == 0.0 ? 1.0 : std::sin(pi * u) / (pi * u);
            const double r = u / kSincZeroes;
            table_[i] = static_cast<float>(s * bessel_i0(kKaiserBeta * std::sqrt(1.0 - r * r)) / i0b);
        }
    }
    float at(double u) const {
        const double pos = (u + kSincZeroes) * kTableRes;
        if (pos < 0.0) return 0.0f;
        const std::size_t i = static_cast<std::size_t>(pos);
        if (i + 1 >= table_.size()) return 0.0f;
        const float f = static_cast<float>(pos - static_cast<double>(i));
        return table_[i] + (table_[i + 1] - table_[i]) * f;
    }

private:
    std::vector<float> table_;
};

}  // namespace

// --- Md5 -------------------------------------------------------------------------

namespace {
const uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
const int kMd5R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                       5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                       4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                       6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
}  // namespace

Md5::Md5() : s_{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476}, buf_{} {}

void Md5::block(const uint8_t* p) {
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = static_cast<uint32_t>(p[4 * i]) | (static_cast<uint32_t>(p[4 * i + 1]) << 8) |
               (static_cast<uint32_t>(p[4 * i + 2]) << 16) | (static_cast<uint32_t>(p[4 * i + 3]) << 24);
    }
    uint32_t a = s_[0], b = s_[1], c = s_[2], d = s_[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7 * i) % 16; }
        const uint32_t t = d;
        d = c;
        c = b;
        const uint32_t x = a + f + kMd5K[i] + m[g];
        b = b + ((x << kMd5R[i]) | (x >> (32 - kMd5R[i])));
        a = t;
    }
    s_[0] += a;
    s_[1] += b;
    s_[2] += c;
    s_[3] += d;
}

void Md5::update(const uint8_t* data, std::size_t n) {
    bytes_ += n;
    while (n > 0) {
        const std::size_t k = std::min(n, sizeof buf_ - fill_);
        std::memcpy(buf_ + fill_, data, k);
        fill_ += k;
        data += k;
        n -= k;
        if (fill_ == sizeof buf_) {
            block(buf_);
            fill_ = 0;
        }
    }
}

void Md5::finish(uint8_t out[16]) {
    const uint64_t bits = bytes_ * 8;
    const uint8_t one = 0x80;
    update(&one, 1);
    const uint8_t zero = 0;
    while (fill_ != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(bits >> (8 * i));
    update(len, 8);
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) out[4 * i + j] = static_cast<uint8_t>(s_[i] >> (8 * j));
    }
}

// --- Quantiser ----------------------------------------------------------------------

Quantiser::Quantiser(int bits, ExportDither dither, int channels, uint32_t seed)
    : bits_(bits), dither_(dither), rng_(seed != 0 ? seed : 1u) {
    (void)channels;
}

float Quantiser::next_uniform() {
    // xorshift32: the dither only has to be flat and not repeat within a file.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<float>(rng_ >> 8) * (1.0f / 16777216.0f);
}

int32_t Quantiser::process(int channel, float x) {
    const int ch = std::min(std::max(channel, 0), 7);
    // A NaN would reach the integer cast; silence is the least bad sample.
    if (!std::isfinite(x)) x = 0.0f;
    const float scale = static_cast<float>(int64_t{1} << (bits_ - 1));
    float v = x * scale;
    if (dither_ == ExportDither::Shaped) v -= kShapeC1 * e1_[ch] + kShapeC2 * e2_[ch];
    float d = 0.0f;
    if (dither_ != ExportDither::None) d = next_uniform() - next_uniform();
    float q = std::nearbyint(v + d);
    q = std::min(std::max(q, -scale), scale - 1.0f);
    if (dither_ == ExportDither::Shaped) {
        // After a clip the error is not noise to be shaped; bound it.
        const float e = std::min(std::max(q - v, -2.0f), 2.0f);
        e2_[ch] = e1_[ch];
        e1_[ch] = e;
    }
    return static_cast<int32_t>(q);
}

// --- export ---------------------------------------------------------------------------

const char* export_extension(ExportContainer c) {
    switch (c) {
        case ExportContainer::Aiff: return ".aiff";
        case ExportContainer::Flac: return ".flac";
        default: return ".wav";
    }
}

std::string export_spec_problem(const ExportSpec& spec) {
    if (spec.bits != 16 && spec.bits != 24 && spec.bits != 32) {
        return "the bit depth is 16, 24 or 32-bit float";
    }
    if (spec.bits == 32 && (spec.container == ExportContainer::Flac ||
                            spec.container == ExportContainer::Aiff)) {
        return std::string(spec.container == ExportContainer::Flac ? "FLAC" : "AIFF") +
               " holds 16 or 24 bits, not 32-bit float";
    }
    if (spec.sample_rate != 0 && (spec.sample_rate < 8000 || spec.sample_rate > 384000)) {
        return "the sample rate is between 8000 and 384000 Hz";
    }
    return {};
}

bool export_audio(const std::string& src, const std::string& dst, const ExportSpec& spec,
                  const ExportMeta& meta, double gain_db, std::string& error) {
    error = export_spec_problem(spec);
    if (!error.empty()) return false;
    WavReader in;
    std::string why;
    if (!in.open(src, why)) {
        error = src + ": " + why;
        return false;
    }
    const uint16_t ch = in.channels();
    if (ch < 1 || ch > 2) {
        error = "export writes mono or stereo files";
        return false;
    }
    const uint32_t in_rate = in.sample_rate();
    const uint32_t out_rate = spec.sample_rate != 0 ? spec.sample_rate : in_rate;
    const bool convert = out_rate != in_rate;
    const int64_t in_frames = in.frames();
    const int64_t out_frames =
        convert ? static_cast<int64_t>(std::llround(static_cast<double>(in_frames) * out_rate / in_rate))
                : in_frames;
    ExportMeta m = meta;
    // The origin is a position in samples; it follows the rate.
    if (convert) {
        m.time_reference = static_cast<uint64_t>(
            std::llround(static_cast<double>(meta.time_reference) * out_rate / in_rate));
    }

    OutFile out;
    if (!out.open(dst, spec.container, spec.bits, out_rate, ch, m, error)) return false;
    const auto fail = [&](const std::string& because) {
        out.abort();
        std::remove(dst.c_str());
        error = because;
        return false;
    };

    const float gain = static_cast<float>(std::pow(10.0, gain_db / 20.0));
    const double step = static_cast<double>(in_rate) / static_cast<double>(out_rate);
    const double fc = std::min(1.0, static_cast<double>(out_rate) / in_rate);
    const double halfw = kSincZeroes / fc;
    Kernel kernel;
    Quantiser quant(spec.bits == 32 ? 24 : spec.bits, spec.dither, ch);
    std::vector<float> win, samples(kBlockFrames * ch);
    std::vector<int32_t> ints(kBlockFrames * ch);

    for (int64_t at = 0; at < out_frames; at += static_cast<int64_t>(kBlockFrames)) {
        const std::size_t k =
            static_cast<std::size_t>(std::min<int64_t>(kBlockFrames, out_frames - at));
        if (!convert) {
            if (in.read(at, static_cast<int64_t>(k), samples.data()) != static_cast<int64_t>(k)) {
                return fail(src + " cannot be read back to export it");
            }
        } else {
            const int64_t lo = static_cast<int64_t>(std::floor(static_cast<double>(at) * step - halfw)) + 1;
            const int64_t hi = static_cast<int64_t>(
                std::floor(static_cast<double>(at + static_cast<int64_t>(k) - 1) * step + halfw));
            win.assign(static_cast<std::size_t>(hi - lo + 1) * ch, 0.0f);
            const int64_t from = std::max<int64_t>(lo, 0);
            const int64_t to = std::min<int64_t>(hi + 1, in_frames);
            if (to > from) {
                if (in.read(from, to - from, win.data() + static_cast<std::size_t>(from - lo) * ch) !=
                    to - from) {
                    return fail(src + " cannot be read back to export it");
                }
            }
            for (std::size_t i = 0; i < k; ++i) {
                const double t = static_cast<double>(at + static_cast<int64_t>(i)) * step;
                const int64_t j0 = static_cast<int64_t>(std::floor(t - halfw)) + 1;
                const int64_t j1 = static_cast<int64_t>(std::floor(t + halfw));
                double a0 = 0.0, a1 = 0.0;
                for (int64_t j = j0; j <= j1; ++j) {
                    const double w = kernel.at((static_cast<double>(j) - t) * fc);
                    const std::size_t p = static_cast<std::size_t>(j - lo) * ch;
                    a0 += w * win[p];
                    if (ch == 2) a1 += w * win[p + 1];
                }
                samples[i * ch] = static_cast<float>(a0 * fc);
                if (ch == 2) samples[i * ch + 1] = static_cast<float>(a1 * fc);
            }
        }
        for (std::size_t i = 0; i < k * ch; ++i) samples[i] *= gain;
        if (spec.bits == 32) {
            if (!out.write_float(samples.data(), k, error)) return fail(error);
        } else {
            for (std::size_t i = 0; i < k; ++i) {
                for (uint16_t c = 0; c < ch; ++c) {
                    ints[i * ch + c] = quant.process(c, samples[i * ch + c]);
                }
            }
            if (!out.write_int(ints.data(), k, error)) return fail(error);
        }
    }
    if (!out.close(error)) return fail(error);
    return true;
}

}  // namespace sonora
