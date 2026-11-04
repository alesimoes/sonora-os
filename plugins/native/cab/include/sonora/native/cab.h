// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cabinet (ADR-0011 stage N3, §4.3): a mono impulse response convolved with
// the input by the zero-latency partitioned convolver of common/dsp
// (Gardner 1995, Wefers 2015: see convolver.h), the same response on every
// channel. Latency is 0, so the cabinet adds nothing to delay compensation.
//
// The response is data, not a parameter. It is loaded by a control thread,
// never the audio thread:
// - load_ir() takes samples and their rate, resamples them to the session
//   rate (a Kaiser-windowed sinc, offline), transforms them into the idle
//   one of two kernels and hands that over with a release store. The audio
//   thread takes it with an acquire load at a block boundary, fades the
//   wet path out over kFadeMs, swaps, holds it at zero for one partition
//   (the old kernel's tail for the block in flight), and fades in. It returns the old
//   kernel to the idle slot; nothing is allocated or freed in process().
//   A load while the previous one is still being taken over returns Busy
//   (retry on the next control tick): the audio thread may be reading the
//   other kernel, and a kernel it may read is never set().
// - Why a dip and not a crossfade of two outputs: a second output needs a
//   second convolver with the input history of the first, i.e. twice the
//   cost all the time, or an incomplete tail during the fade. A response
//   that changes is a deliberate edit; about 2 x kFadeMs + 3 ms of dip is inaudible as a
//   click and the old tail is not carried into the new response.
// - Responses longer than kMaxSeconds are cut and faded over their last
//   10 ms (LoadResult::Truncated). Before any response is loaded the
//   cabinet is a unit impulse.
//
// What it does not do yet: the response chosen through plugin state. The
// state is a canonical JSON of parameter values (native/state.h); a file
// reference needs the ADR-0029 §6 path mapping, which the SDK and the
// format layer do not carry. See the PR.
#pragma once

#include <cstddef>
#include <memory>

#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace cab {
enum : std::uint32_t {
    kBypass = 0,
    kOutput,
    kMix,
    kParamCount,
};
constexpr double kMaxSeconds = 1.0;
constexpr double kFadeMs = 4.0;
}  // namespace cab

class Cab : public ProcessorBase {
public:
    enum class LoadResult {
        Ok,
        Truncated,    // loaded, cut to kMaxSeconds
        Busy,         // the previous load is not taken over yet: retry
        Invalid,      // empty, non-finite beyond use, or a bad rate
    };

    // Not for the audio thread. Safe to call from one control thread while
    // process() runs; before prepare() it is kept and applied by prepare().
    // `ir` is mono at `ir_rate` Hz.
    virtual LoadResult load_ir(const float* ir, std::size_t frames, double ir_rate) = 0;

protected:
    Cab(const Descriptor& d) : ProcessorBase(d) {}
};

std::unique_ptr<Cab> make_cab();

}  // namespace sonora::native
