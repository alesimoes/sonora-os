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
// The response is also the cabinet's one file slot, "ir" (native/files.h,
// ADR-0029 §6): a plugin state carries its path, the host places the file,
// and files()->load() reads it on a control thread through the reader that
// make_cab_plugin() (cab_file.h) installs. A cab from make_cab() has no
// reader and so no slot: the neutral build has no WAV reader. Loading an
// empty path puts the unit impulse back.
//
// "Trim" and "Normalise" (state version 2, both off by default) condition
// the loaded response with dsp/ir_prepare.h: Trim drops the leading and
// trailing near-silence (a fixed delay the player would hear), Normalise
// scales to unit energy. They apply to the response as recorded, which the
// cab keeps, so they take effect at the next prepare() (`reconfigures`: the
// plugin bridge re-prepares) and need no file read. A version 1 state has
// neither key: both load as off, so an old session sounds exactly as it did.
#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "sonora/native/processor_base.h"

namespace sonora::native {

namespace cab {
enum : std::uint32_t {
    kBypass = 0,
    kOutput,
    kMix,
    kTrim,       // state version 2
    kNormalise,  // state version 2
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

    // Reads the file of the "ir" slot into a Cab (load_cab_ir_wav is one).
    using IrReader = LoadResult (*)(Cab& cab, const std::string& path, std::string& error);
    // Turns the file slot on. Before the cab is shared with another thread.
    virtual void set_ir_reader(IrReader reader) = 0;

protected:
    Cab(const Descriptor& d) : ProcessorBase(d) {}
};

std::unique_ptr<Cab> make_cab();

// A cabinet with its "ir" file slot on: files()->load() reads a WAV through
// load_cab_ir_wav(). The factory of the LV2 and VST3 bundles, whose state
// carries the response's path. Defined in sonora_native_cab_file (cab_file.cpp),
// not in this library, which has no WAV reader.
std::unique_ptr<Cab> make_cab_plugin();

}  // namespace sonora::native
