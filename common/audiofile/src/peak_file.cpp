// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/peak_file.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "sonora/wav_reader.h"

namespace sonora {

namespace {

constexpr char kMagic[4] = {'S', 'P', 'K', '1'};

int16_t to_i16(float v) {
    return static_cast<int16_t>(std::lrintf(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
}

void put32(std::FILE* f, uint32_t v) {
    const unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
                                static_cast<unsigned char>(v >> 16),
                                static_cast<unsigned char>(v >> 24)};
    std::fwrite(b, 1, 4, f);
}

bool get32(std::FILE* f, uint32_t& v) {
    unsigned char b[4];
    if (std::fread(b, 1, 4, f) != 4) return false;
    v = b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<uint32_t>(b[3]) << 24);
    return true;
}

int64_t level_buckets(int64_t frames, uint32_t level) {
    int64_t size = kPeakBase;
    for (uint32_t i = 0; i < level; ++i) size *= kPeakFactor;
    return (frames + size - 1) / size;
}

}  // namespace

std::string peak_file_path(const std::string& session_dir, const std::string& source_path) {
    return session_dir + "/Peaks/" + source_path + ".spk";
}

PeakBuilder::PeakBuilder(uint16_t channels)
    : channels_(channels), level0_(channels), lo_(channels, 0.0f), hi_(channels, 0.0f) {}

void PeakBuilder::add(const float* in, int64_t frames) {
    for (int64_t f = 0; f < frames; ++f) {
        for (uint16_t c = 0; c < channels_; ++c) {
            const float v = in[f * channels_ + c];
            if (in_bucket_ == 0) {
                lo_[c] = hi_[c] = v;
            } else {
                lo_[c] = std::min(lo_[c], v);
                hi_[c] = std::max(hi_[c], v);
            }
        }
        ++frames_;
        if (++in_bucket_ == kPeakBase) {
            for (uint16_t c = 0; c < channels_; ++c) {
                level0_[c].push_back(to_i16(lo_[c]));
                level0_[c].push_back(to_i16(hi_[c]));
            }
            in_bucket_ = 0;
        }
    }
}

bool PeakBuilder::write(const std::string& path, std::string& error) const {
    // The partial bucket at the end is a bucket too.
    std::vector<std::vector<int16_t>> cur = level0_;
    if (in_bucket_ > 0) {
        for (uint16_t c = 0; c < channels_; ++c) {
            cur[c].push_back(to_i16(lo_[c]));
            cur[c].push_back(to_i16(hi_[c]));
        }
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path target(path);
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) {
        error = "cannot create " + tmp;
        return false;
    }
    std::fwrite(kMagic, 1, 4, f);
    put32(f, 1);
    put32(f, channels_);
    put32(f, kPeakBase);
    put32(f, kPeakFactor);
    put32(f, kPeakLevels);
    put32(f, static_cast<uint32_t>(static_cast<uint64_t>(frames_)));
    put32(f, static_cast<uint32_t>(static_cast<uint64_t>(frames_) >> 32));
    for (uint32_t level = 0; level < kPeakLevels; ++level) {
        for (uint16_t c = 0; c < channels_; ++c) {
            for (const int16_t v : cur[c]) {
                const uint16_t u = static_cast<uint16_t>(v);
                const unsigned char b[2] = {static_cast<unsigned char>(u),
                                            static_cast<unsigned char>(u >> 8)};
                std::fwrite(b, 1, 2, f);
            }
        }
        if (level + 1 == kPeakLevels) break;
        for (uint16_t c = 0; c < channels_; ++c) {
            std::vector<int16_t> next;
            const std::vector<int16_t>& src = cur[c];
            for (std::size_t i = 0; i < src.size(); i += 2 * kPeakFactor) {
                int16_t lo = src[i], hi = src[i + 1];
                for (std::size_t k = i + 2; k < std::min(src.size(), i + 2 * kPeakFactor);
                     k += 2) {
                    lo = std::min(lo, src[k]);
                    hi = std::max(hi, src[k + 1]);
                }
                next.push_back(lo);
                next.push_back(hi);
            }
            cur[c] = std::move(next);
        }
    }
    const bool wrote = std::fflush(f) == 0 && !std::ferror(f);
    std::fclose(f);
    if (!wrote) {
        std::remove(tmp.c_str());
        error = "cannot write " + tmp;
        return false;
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        std::remove(tmp.c_str());
        error = "cannot rename " + tmp + ": " + ec.message();
        return false;
    }
    return true;
}

bool build_peak_file(const std::string& wav, const std::string& peak, std::string& error) {
    WavReader r;
    if (!r.open(wav, error)) return false;
    PeakBuilder b(r.channels());
    constexpr int64_t kChunk = 1 << 16;
    std::vector<float> buf(static_cast<std::size_t>(kChunk) * r.channels());
    for (int64_t at = 0; at < r.frames();) {
        const int64_t n = r.read(at, std::min(kChunk, r.frames() - at), buf.data());
        if (n <= 0) {
            error = "cannot read " + wav;
            return false;
        }
        b.add(buf.data(), n);
        at += n;
    }
    return b.write(peak, error);
}

bool PeakFile::open(const std::string& path, std::string& error) {
    data_.clear();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        error = "no peak file " + path;
        return false;
    }
    const auto fail = [&](const char* why) {
        std::fclose(f);
        data_.clear();
        error = std::string(why) + ": " + path;
        return false;
    };
    char magic[4];
    uint32_t version = 0, channels = 0, base = 0, factor = 0, levels = 0, lo = 0, hi = 0;
    if (std::fread(magic, 1, 4, f) != 4 || std::memcmp(magic, kMagic, 4) != 0) {
        return fail("not a peak file");
    }
    if (!get32(f, version) || !get32(f, channels) || !get32(f, base) || !get32(f, factor) ||
        !get32(f, levels) || !get32(f, lo) || !get32(f, hi)) {
        return fail("peak file cut short");
    }
    if (version != 1 || base != kPeakBase || factor != kPeakFactor || levels != kPeakLevels ||
        channels == 0 || channels > 4096) {
        return fail("peak file of another layout");
    }
    channels_ = static_cast<uint16_t>(channels);
    frames_ = static_cast<int64_t>((static_cast<uint64_t>(hi) << 32) | lo);
    data_.assign(levels, std::vector<std::vector<int16_t>>(channels_));
    for (uint32_t level = 0; level < levels; ++level) {
        const std::size_t n = static_cast<std::size_t>(level_buckets(frames_, level)) * 2;
        for (uint16_t c = 0; c < channels_; ++c) {
            std::vector<unsigned char> raw(n * 2);
            if (!raw.empty() && std::fread(raw.data(), 1, raw.size(), f) != raw.size()) {
                return fail("peak file cut short");
            }
            std::vector<int16_t>& d = data_[level][c];
            d.resize(n);
            for (std::size_t i = 0; i < n; ++i) {
                d[i] = static_cast<int16_t>(raw[2 * i] | (raw[2 * i + 1] << 8));
            }
        }
    }
    std::fclose(f);
    return true;
}

void PeakFile::read(uint16_t channel, int64_t first, double frames_per_bucket, int count,
                    float* min, float* max) const {
    for (int i = 0; i < count; ++i) min[i] = max[i] = 0.0f;
    if (data_.empty() || channel >= channels_) return;
    const double fpb = std::max(1.0, frames_per_bucket);
    // The coarsest level whose bucket is no wider than the one asked for.
    uint32_t level = 0;
    double size = kPeakBase;
    while (level + 1 < kPeakLevels && size * kPeakFactor <= fpb) {
        size *= kPeakFactor;
        ++level;
    }
    const std::vector<int16_t>& d = data_[level][channel];
    const int64_t buckets = static_cast<int64_t>(d.size() / 2);
    for (int i = 0; i < count; ++i) {
        const double a = static_cast<double>(first) + i * fpb;
        const double b = a + fpb;
        if (b <= 0 || a >= static_cast<double>(frames_)) continue;
        const int64_t k0 = std::max<int64_t>(0, static_cast<int64_t>(std::floor(a / size)));
        const int64_t k1 =
            std::min<int64_t>(buckets, static_cast<int64_t>(std::ceil(b / size)));
        int16_t lo = 0, hi = 0;
        bool any = false;
        for (int64_t k = k0; k < k1; ++k) {
            const int16_t l = d[static_cast<std::size_t>(2 * k)];
            const int16_t h = d[static_cast<std::size_t>(2 * k + 1)];
            lo = any ? std::min(lo, l) : l;
            hi = any ? std::max(hi, h) : h;
            any = true;
        }
        if (any) {
            min[i] = static_cast<float>(lo) / 32767.0f;
            max[i] = static_cast<float>(hi) / 32767.0f;
        }
    }
}

}  // namespace sonora
