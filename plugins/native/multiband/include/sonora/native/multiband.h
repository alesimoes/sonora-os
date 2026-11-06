// SPDX-License-Identifier: GPL-3.0-or-later
//
// Multiband compressor (plugins.stock) for the mix bus and mastering of
// analog-recorded material: three or four bands, each a compressor of
// common/dsp's log-domain kind (Giannoulis, Massberg and Reiss, JAES 60(6),
// 2012: soft-knee gain computer, branching peak detector), with its own
// threshold, ratio, attack, release, make-up, solo and bypass.
//
// The bands come from one of two crossovers (common/dsp/crossover.h):
// - minimum phase: Linkwitz-Riley fourth-order, summing flat in magnitude
//   (an all-pass overall), no buffering latency;
// - linear phase: windowed-sinc FIRs whose bands sum to the input delayed by
//   exactly the reported latency (1024 samples to 48 kHz, 2048 above).
// An optional look-ahead delays the audio, not the detectors, by 0 to 10 ms,
// added to the reported latency in either mode. The crossovers, the band
// count, the phase mode and the look-ahead change only at prepare().
//
// Each band's gain reduction is kept in the processor (BandMeters) for the
// plugin telemetry region to publish; nothing here depends on that contract.
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_multiband();

// The per-band gain reduction, in dB (positive: reduced), of the last block;
// what the telemetry region will carry. Reached by dynamic_cast on the
// Processor make_multiband() returns. Safe to read from another thread.
class BandMeters {
public:
    static constexpr std::uint32_t kMaxBands = 4;
    virtual ~BandMeters() = default;
    virtual std::uint32_t band_count() const noexcept = 0;
    virtual float band_gr_db(std::uint32_t band) const noexcept = 0;
};

namespace multiband {
enum : std::uint32_t {
    kBypass = 0,
    kBands,  // choice: 3 bands, 4 bands
    kPhase,  // choice: minimum, linear
    kX1,
    kX2,
    kX3,
    kLookahead,
    kKnee,
    kScHpfOn,
    kScHpfFreq,
    kAutoMakeup,
    kLink,
    kOutGain,
    kBandBase,  // then 7 per band, band-major
};
enum : std::uint32_t {
    kThreshold = 0,
    kRatio,
    kAttack,
    kRelease,
    kMakeup,
    kSolo,
    kBandBypass,
    kPerBand,
};
constexpr std::uint32_t kMaxBands = 4;
constexpr std::uint32_t kParamCount = kBandBase + kMaxBands * kPerBand;
constexpr std::uint32_t band_param(std::uint32_t band, std::uint32_t which) {
    return kBandBase + band * kPerBand + which;
}
}  // namespace multiband

}  // namespace sonora::native
