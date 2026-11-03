// SPDX-License-Identifier: GPL-3.0-or-later
//
// Zero-latency partitioned convolution (ADR-0011 §2.1, "partitioned
// convolution"; stage N3), for cabinet impulse responses (§4.3) and any
// other fixed FIR longer than a direct loop can afford.
//
// Design, from the literature:
// - W. G. Gardner, "Efficient convolution without input-output delay",
//   JAES 43(3), 1995: split the response into a head run directly in the
//   time domain and a tail run by block FFT convolution, so the block
//   transform's delay is hidden behind the head and the output has no
//   latency at all. Here the head is the first P taps and the tail is
//   uniformly partitioned in blocks of P: a block of input completed at
//   sample n is needed by the tail only from sample n + 1 on, exactly one
//   block later, which is the delay the block transform has.
// - F. Wefers, "Partitioned convolution algorithms for real-time
//   auralization", PhD thesis, RWTH Aachen, 2015, §5 (uniformly
//   partitioned overlap-save, "UPOLS"): the tail's partitions are
//   transformed once at set time; each input block is transformed once
//   into a frequency-domain delay line (FDL) and multiplied with every
//   partition, so a block costs one forward and one inverse transform of
//   2P points plus K (P + 1) complex multiply-adds for K partitions.
//   The transforms are real (RealFft, fft.h); the multiply-adds and the
//   head's dot product are SSE2 on x86-64 (src/arch/, convolver_kernels.h).
//
// Our choices, each measured in tests/test_convolver.cpp:
// - Double precision throughout: against a direct double convolution the
//   worst error measured is -145 dB of the peak, which is the float
//   output's own rounding.
// - Zero latency, so a cabinet adds nothing to plugin delay compensation:
//   latency() is 0 and the test measures it at every block size.
// - The convolver's state (the head's input history, the FDL of input
//   spectra) does not depend on the response. A kernel may therefore be
//   swapped between two process() calls without a reset: the head is exact
//   at once and the tail at the next block boundary, at most P samples
//   later. A processor that changes IR while playing crossfades over that.
// - The work is uneven: the block transform runs on the sample that
//   completes a block of P. With the host's quantum a multiple of P that
//   is the same number of transforms in every call.
#pragma once

#include <cstdint>
#include <vector>

#include "sonora/dsp/fft.h"
#include "sonora/rt.h"

namespace sonora::dsp {

// An impulse response prepared for a Convolver: the head taps and the
// spectra of the tail's partitions. Built off the audio thread.
class ConvolutionKernel {
public:
    // partition: P, a power of two from 16 to 4096. max_taps bounds every
    // later set(). Allocates.
    bool prepare(std::uint32_t partition, std::uint32_t max_taps);

    // Copies and transforms h[0..taps). Allocates nothing; O(taps log P),
    // so it is for a loader thread, not for process(). Refused (false, the
    // kernel unchanged) when taps is 0 or over max_taps, or when a tap is
    // not finite. After prepare() the kernel is a unit impulse.
    bool set(const double* h, std::uint32_t taps) noexcept;

    std::uint32_t partition() const noexcept { return p_; }
    std::uint32_t taps() const noexcept { return taps_; }
    std::uint32_t max_taps() const noexcept { return max_taps_; }
    // Tail partitions in use: ceil((taps - P) / P), 0 when taps <= P.
    std::uint32_t tail_partitions() const noexcept { return parts_; }

private:
    friend class Convolver;
    std::uint32_t p_ = 0, taps_ = 0, max_taps_ = 0, parts_ = 0, max_parts_ = 0;
    std::vector<double> head_;      // h[P-1] ... h[0], reversed for the dot product
    std::vector<double> re_, im_;   // partition i, bin k at i * (P + 1) + k
    RealFft fft_;
    std::vector<double> block_;                  // 2P
    std::vector<std::complex<double>> spectrum_;  // P + 1
};

// One input, one output, one kernel at a time.
class Convolver {
public:
    // partition must equal the kernels' partition; max_taps bounds their
    // length. Allocates.
    bool prepare(std::uint32_t partition, std::uint32_t max_taps);
    void reset() noexcept;
    std::uint32_t partition() const noexcept { return p_; }
    static constexpr std::uint32_t latency() noexcept { return 0; }

    // out = (in * h)[n] for the kernel's h. out may alias in. A non-finite
    // input sample is read as 0, so the output stays finite. A kernel
    // prepared for another partition size, or longer than this convolver
    // was prepared for, gives silence (the loader checks with accepts()).
    void process(const ConvolutionKernel& kernel, const float* in, float* out,
                 std::uint32_t frames) noexcept SONORA_RT;

    bool accepts(const ConvolutionKernel& kernel) const noexcept {
        return kernel.p_ == p_ && kernel.max_parts_ <= max_parts_;
    }

private:
    void run_block(const ConvolutionKernel& kernel) noexcept SONORA_RT;

    std::uint32_t p_ = 0, max_parts_ = 0;
    std::uint32_t pos_ = 0;     // samples into the current block
    std::uint32_t newest_ = 0;  // FDL slot of the newest input spectrum
    // The last P inputs, doubled so the head's dot product is contiguous:
    // hist_[hpos_ .. hpos_ + P) is oldest to newest.
    std::vector<double> hist_;
    std::uint32_t hpos_ = 0;
    std::vector<double> block_;     // the previous and the current block, 2P
    std::vector<double> tail_out_;  // the tail's output for this block, P
    std::vector<double> fdl_re_, fdl_im_;  // max_parts_ spectra of P + 1 bins
    std::vector<double> acc_re_, acc_im_;
    std::vector<std::complex<double>> spectrum_;  // P + 1
    std::vector<double> time_;                    // 2P
    RealFft fft_;
};

}  // namespace sonora::dsp
