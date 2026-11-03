// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/dsp/convolver.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "convolver_kernels.h"
#include "sonora/dsp/math.h"

namespace sonora::dsp {

namespace {

constexpr double kFloatMax = static_cast<double>(std::numeric_limits<float>::max());

bool valid_partition(std::uint32_t p) {
    return p >= 16 && p <= 4096 && (p & (p - 1)) == 0;
}

std::uint32_t tail_parts(std::uint32_t taps, std::uint32_t p) {
    return taps <= p ? 0 : (taps - p + p - 1) / p;
}

}  // namespace

bool ConvolutionKernel::prepare(std::uint32_t partition, std::uint32_t max_taps) {
    if (!valid_partition(partition) || max_taps == 0) return false;
    if (!fft_.prepare(2 * partition)) return false;
    p_ = partition;
    max_taps_ = max_taps;
    max_parts_ = tail_parts(max_taps, partition);
    head_.assign(partition, 0.0);
    const std::size_t bins = static_cast<std::size_t>(max_parts_) * (partition + 1);
    re_.assign(bins, 0.0);
    im_.assign(bins, 0.0);
    block_.assign(2 * static_cast<std::size_t>(partition), 0.0);
    spectrum_.assign(partition + 1, {0.0, 0.0});
    const double unit = 1.0;
    set(&unit, 1);
    return true;
}

bool ConvolutionKernel::set(const double* h, std::uint32_t taps) noexcept {
    if (p_ == 0 || taps == 0 || taps > max_taps_ || h == nullptr) return false;
    for (std::uint32_t i = 0; i < taps; ++i) {
        if (!is_finite(h[i])) return false;
    }
    taps_ = taps;
    parts_ = tail_parts(taps, p_);
    for (std::uint32_t j = 0; j < p_; ++j) {
        const std::uint32_t tap = p_ - 1 - j;
        head_[j] = tap < taps ? h[tap] : 0.0;
    }
    for (std::uint32_t i = 0; i < parts_; ++i) {
        const std::uint32_t first = p_ + i * p_;
        for (std::uint32_t j = 0; j < 2 * p_; ++j) {
            const std::uint32_t tap = first + j;
            block_[j] = j < p_ && tap < taps ? h[tap] : 0.0;
        }
        fft_.forward(block_.data(), spectrum_.data());
        double* re = re_.data() + static_cast<std::size_t>(i) * (p_ + 1);
        double* im = im_.data() + static_cast<std::size_t>(i) * (p_ + 1);
        for (std::uint32_t k = 0; k <= p_; ++k) {
            re[k] = spectrum_[k].real();
            im[k] = spectrum_[k].imag();
        }
    }
    return true;
}

bool Convolver::prepare(std::uint32_t partition, std::uint32_t max_taps) {
    if (!valid_partition(partition) || max_taps == 0) return false;
    if (!fft_.prepare(2 * partition)) return false;
    p_ = partition;
    // At least one slot: the FDL is kept even for a response that fits in
    // the head, so a longer kernel swapped in later still sees the history.
    max_parts_ = std::max<std::uint32_t>(1, tail_parts(max_taps, partition));
    hist_.assign(2 * static_cast<std::size_t>(partition), 0.0);
    block_.assign(2 * static_cast<std::size_t>(partition), 0.0);
    tail_out_.assign(partition, 0.0);
    const std::size_t bins = static_cast<std::size_t>(max_parts_) * (partition + 1);
    fdl_re_.assign(bins, 0.0);
    fdl_im_.assign(bins, 0.0);
    acc_re_.assign(partition + 1, 0.0);
    acc_im_.assign(partition + 1, 0.0);
    spectrum_.assign(partition + 1, {0.0, 0.0});
    time_.assign(2 * static_cast<std::size_t>(partition), 0.0);
    reset();
    return true;
}

void Convolver::reset() noexcept {
    std::fill(hist_.begin(), hist_.end(), 0.0);
    std::fill(block_.begin(), block_.end(), 0.0);
    std::fill(tail_out_.begin(), tail_out_.end(), 0.0);
    std::fill(fdl_re_.begin(), fdl_re_.end(), 0.0);
    std::fill(fdl_im_.begin(), fdl_im_.end(), 0.0);
    pos_ = 0;
    hpos_ = 0;
    newest_ = 0;
}

void Convolver::run_block(const ConvolutionKernel& kernel) noexcept {
    const std::uint32_t bins = p_ + 1;
    fft_.forward(block_.data(), spectrum_.data());
    newest_ = newest_ + 1 == max_parts_ ? 0 : newest_ + 1;
    {
        double* re = fdl_re_.data() + static_cast<std::size_t>(newest_) * bins;
        double* im = fdl_im_.data() + static_cast<std::size_t>(newest_) * bins;
        for (std::uint32_t k = 0; k < bins; ++k) {
            re[k] = spectrum_[k].real();
            im[k] = spectrum_[k].imag();
        }
    }
    std::copy(block_.begin() + p_, block_.end(), block_.begin());

    const std::uint32_t parts = kernel.parts_;
    if (parts == 0) {
        std::fill(tail_out_.begin(), tail_out_.end(), 0.0);
        return;
    }
    std::fill(acc_re_.begin(), acc_re_.end(), 0.0);
    std::fill(acc_im_.begin(), acc_im_.end(), 0.0);
    double* const ar = acc_re_.data();
    double* const ai = acc_im_.data();
    // Partition i meets the input spectrum i blocks old, two partitions a
    // pass.
    const auto x_re = [&](std::uint32_t s) { return fdl_re_.data() + static_cast<std::size_t>(s) * bins; };
    const auto x_im = [&](std::uint32_t s) { return fdl_im_.data() + static_cast<std::size_t>(s) * bins; };
    const auto h_re = [&](std::uint32_t i) { return kernel.re_.data() + static_cast<std::size_t>(i) * bins; };
    const auto h_im = [&](std::uint32_t i) { return kernel.im_.data() + static_cast<std::size_t>(i) * bins; };
    const auto older = [&](std::uint32_t s) { return s == 0 ? max_parts_ - 1 : s - 1; };
    std::uint32_t slot = newest_;
    std::uint32_t i = 0;
    for (; i + 2 <= parts; i += 2) {
        const std::uint32_t next = older(slot);
        spectral_mac2(ar, ai, x_re(slot), x_im(slot), h_re(i), h_im(i), x_re(next), x_im(next), h_re(i + 1),
                      h_im(i + 1), bins);
        slot = older(next);
    }
    if (i < parts) spectral_mac(ar, ai, x_re(slot), x_im(slot), h_re(i), h_im(i), bins);
    for (std::uint32_t k = 0; k < bins; ++k) spectrum_[k] = {ar[k], ai[k]};
    fft_.inverse(spectrum_.data(), time_.data());
    // Overlap-save: the last P points are the linear convolution.
    std::copy(time_.begin() + p_, time_.end(), tail_out_.begin());
}

void Convolver::process(const ConvolutionKernel& kernel, const float* in, float* out,
                        std::uint32_t frames) noexcept {
    if (kernel.p_ != p_ || p_ == 0 || kernel.parts_ > max_parts_) {
        std::fill(out, out + frames, 0.0f);
        return;
    }
    const double* const head = kernel.head_.data();
    for (std::uint32_t n = 0; n < frames; ++n) {
        const float xf = in[n];
        const double x = is_finite(xf) ? static_cast<double>(xf) : 0.0;
        hist_[hpos_] = x;
        hist_[hpos_ + p_] = x;
        hpos_ = hpos_ + 1 == p_ ? 0 : hpos_ + 1;
        block_[p_ + pos_] = x;
        const double y = head_dot(head, hist_.data() + hpos_, p_) + tail_out_[pos_];
        // A loud input through a response with gain could pass the float
        // range; held at its edge, the output stays finite.
        out[n] = static_cast<float>(std::clamp(y, -kFloatMax, kFloatMax));
        if (++pos_ == p_) {
            run_block(kernel);
            pos_ = 0;
        }
    }
}

}  // namespace sonora::dsp
