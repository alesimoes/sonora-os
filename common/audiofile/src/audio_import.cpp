// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/audio_import.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>

#include "sonora/wav_reader.h"

namespace sonora {
namespace {

constexpr int kZeroCrossings = 32;
constexpr int kPhases = 1024;
constexpr double kPi = 3.14159265358979323846;
constexpr double kKaiserBeta = 9.0;  // about 90 dB of stopband

double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 60; ++k) {
        const double h = x / (2.0 * k);
        term *= h * h;
        sum += term;
        if (term < 1e-14 * sum) break;
    }
    return sum;
}

}  // namespace

bool probe_audio(const std::string& path, AudioInfo& out, std::string& why) {
    WavReader r;
    if (!r.open(path, why)) return false;
    if (r.frames() <= 0) {
        why = "the file holds no audio";
        return false;
    }
    out.channels = r.channels();
    out.sample_rate = r.sample_rate();
    out.frames = r.frames();
    out.floating = r.encoding() == WavReader::Encoding::Float32;
    out.time_reference = r.time_reference();
    return true;
}

Resampler::Resampler(uint32_t in_rate, uint32_t out_rate, uint16_t channels)
    : in_rate_(in_rate), out_rate_(out_rate), channels_(channels) {
    scale_ = std::max(1.0, static_cast<double>(in_rate) / out_rate);
    half_ = static_cast<int>(std::ceil(kZeroCrossings * scale_));
    // A little below Nyquist, so the transition band stays inside it.
    const double cutoff = (1.0 / scale_) * 0.98;
    const int taps = 2 * half_;
    table_.assign(static_cast<std::size_t>(kPhases + 1) * taps, 0.0f);
    const double i0b = bessel_i0(kKaiserBeta);
    for (int p = 0; p <= kPhases; ++p) {
        const double frac = static_cast<double>(p) / kPhases;
        double sum = 0.0;
        float* row = &table_[static_cast<std::size_t>(p) * taps];
        for (int t = 0; t < taps; ++t) {
            // Tap t sits at input offset (t - half_ + 1) from floor(position).
            const double x = (t - half_ + 1) - frac;
            const double w = x / half_;
            double k = 0.0;
            if (std::fabs(w) < 1.0) {
                const double a = kPi * x * cutoff;
                const double s = std::fabs(a) < 1e-12 ? 1.0 : std::sin(a) / a;
                k = cutoff * s * bessel_i0(kKaiserBeta * std::sqrt(1.0 - w * w)) / i0b;
            }
            row[t] = static_cast<float>(k);
            sum += k;
        }
        // Unity gain at DC for every phase, so no phase modulates the level.
        if (sum != 0.0) {
            for (int t = 0; t < taps; ++t) row[t] = static_cast<float>(row[t] / sum);
        }
    }
}

int64_t Resampler::output_frames(int64_t in_frames) const {
    return static_cast<int64_t>(
        std::llround(static_cast<long double>(in_frames) * out_rate_ / in_rate_));
}

void Resampler::produce(std::vector<float>& out, int64_t limit_in, int64_t total_out) {
    const int taps = 2 * half_;
    const std::size_t ch = channels_;
    while (next_out_ < total_out) {
        const int64_t num = next_out_ * static_cast<int64_t>(in_rate_);
        const int64_t base = num / out_rate_;
        const double frac =
            static_cast<double>(num % out_rate_) / static_cast<double>(out_rate_);
        // The taps reach base + half_; wait until that input has arrived.
        if (base + half_ >= limit_in) break;
        const double fp = frac * kPhases;
        const int p = std::min(static_cast<int>(fp), kPhases - 1);
        const float mix = static_cast<float>(fp - p);
        const float* r0 = &table_[static_cast<std::size_t>(p) * taps];
        const float* r1 = r0 + taps;
        const int64_t first = base - half_ + 1;
        for (std::size_t c = 0; c < ch; ++c) {
            double acc = 0.0;
            for (int t = 0; t < taps; ++t) {
                const int64_t i = first + t;
                if (i < 0 || i >= in_total_) continue;
                const float v = buf_[static_cast<std::size_t>(i - buf_start_) * ch + c];
                acc += v * (r0[t] + mix * (r1[t] - r0[t]));
            }
            out.push_back(static_cast<float>(acc));
        }
        ++next_out_;
    }
    // Forget the input that no later output reaches.
    const int64_t keep_from = std::max<int64_t>(
        0, (next_out_ * static_cast<int64_t>(in_rate_)) / out_rate_ - half_ + 1);
    const int64_t drop = std::min(keep_from, in_total_) - buf_start_;
    if (drop > 0) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(drop * ch));
        buf_start_ += drop;
    }
}

void Resampler::push(const float* interleaved, std::size_t frames, std::vector<float>& out) {
    buf_.insert(buf_.end(), interleaved, interleaved + frames * channels_);
    in_total_ += static_cast<int64_t>(frames);
    produce(out, in_total_, INT64_MAX);
}

void Resampler::finish(int64_t in_frames, std::vector<float>& out) {
    // Beyond the end the input is silence: treat it as having arrived.
    produce(out, INT64_MAX, output_frames(in_frames));
}

bool import_audio(const std::string& src, const std::string& dest, uint32_t rate,
                  std::atomic<int>* progress, const std::atomic<bool>* cancel,
                  ImportResult& out, std::string& why) {
    why.clear();
    WavReader in;
    if (!in.open(src, why)) return false;
    const uint16_t channels = in.channels();
    const int64_t frames = in.frames();
    const uint32_t in_rate = in.sample_rate();
    if (frames <= 0) {
        why = "the file holds no audio";
        return false;
    }
    if (rate == 0 || in_rate == 0) {
        why = "the sample rate is zero";
        return false;
    }
    const bool floating = in.encoding() == WavReader::Encoding::Float32;
    const bool convert = in_rate != rate;
    const auto scaled = [&](int64_t v) {
        return convert ? static_cast<int64_t>(std::llround(static_cast<long double>(v) * rate / in_rate))
                       : v;
    };
    const int64_t out_frames = scaled(frames);
    if (out_frames <= 0) {
        why = "the file is too short to convert";
        return false;
    }
    const int64_t time_ref = scaled(static_cast<int64_t>(in.time_reference()));

    WavWriter w;
    if (!w.open(dest, rate, channels,
                floating ? WavWriter::Format::Float32 : WavWriter::Format::Int24,
                static_cast<uint64_t>(time_ref))) {
        why = "cannot create " + dest + (w.failure().empty() ? "" : ": " + w.failure());
        return false;
    }
    const auto fail = [&](const std::string& reason) {
        w.close();
        std::remove(dest.c_str());
        why = reason;
        return false;
    };

    constexpr int64_t kBlock = 16384;
    std::vector<float> block(static_cast<std::size_t>(kBlock) * channels);
    std::vector<float> converted;
    Resampler rs(in_rate, rate, channels);
    int64_t done = 0;
    int64_t written = 0;
    const auto emit = [&](const std::vector<float>& v) {
        const int64_t have = static_cast<int64_t>(v.size() / channels);
        // The length is the resampler's rounding; never write past it.
        const int64_t n = std::min(have, out_frames - written);
        if (n <= 0) return true;
        if (!w.write(v.data(), static_cast<std::size_t>(n))) return false;
        written += n;
        return true;
    };
    while (done < frames) {
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) return fail(std::string());
        const int64_t want = std::min(kBlock, frames - done);
        const int64_t got = in.read(done, want, block.data());
        if (got <= 0) return fail("the file ends before its header says it does");
        if (convert) {
            converted.clear();
            rs.push(block.data(), static_cast<std::size_t>(got), converted);
            if (!emit(converted)) return fail("write failed: " + w.failure());
        } else {
            if (!w.write(block.data(), static_cast<std::size_t>(got))) {
                return fail("write failed: " + w.failure());
            }
            written += got;
        }
        done += got;
        if (progress != nullptr) {
            progress->store(static_cast<int>(done * 1000 / frames), std::memory_order_relaxed);
        }
    }
    if (convert) {
        converted.clear();
        rs.finish(frames, converted);
        if (!emit(converted)) return fail("write failed: " + w.failure());
    }
    if (!w.close()) return fail("write failed: " + w.failure());
    out.channels = channels;
    out.frames = written;
    out.floating = floating;
    out.time_reference = time_ref;
    out.resampled = convert;
    return true;
}

}  // namespace sonora
