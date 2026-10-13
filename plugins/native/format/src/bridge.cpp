// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/format/bridge.h"

#include <utility>

namespace sonora::native::format {

namespace {
// What run() falls back to before the first activate(), or when the
// Processor refused its layout: a format that runs an inactive instance
// gets silence, never stale or uninitialised memory.
void silence(float* const* out, std::uint32_t channels, std::uint32_t frames) noexcept {
    for (std::uint32_t c = 0; c < channels; ++c) {
        for (std::uint32_t i = 0; i < frames; ++i) out[c][i] = 0.0f;
    }
}
}  // namespace

Bridge::Bridge(std::unique_ptr<Processor> processor, std::uint32_t channels)
    : processor_(std::move(processor)),
      channels_(channels),
      count_(processor_->descriptor().param_count),
      values_(new std::atomic<float>[count_]),
      dirty_(new std::atomic<bool>[count_]),
      events_(new ParamEvent[count_]),
      prepared_values_(new float[count_]()) {
    for (std::uint32_t i = 0; i < count_; ++i) {
        values_[i].store(processor_->param(i), std::memory_order_relaxed);
        dirty_[i].store(false, std::memory_order_relaxed);
    }
}

void Bridge::set_value(std::uint32_t index, float value) noexcept {
    if (index >= count_) return;
    values_[index].store(sanitize(descriptor().params[index], value), std::memory_order_relaxed);
    // Release on both flags: whoever sees one set also sees the value. A
    // value that changes again before run() collects it is simply read
    // later; a flag raised twice delivers the same value twice, which a
    // smoother ignores.
    dirty_[index].store(true, std::memory_order_release);
    any_dirty_.store(true, std::memory_order_release);
}

float Bridge::value(std::uint32_t index) const noexcept {
    return index < count_ ? values_[index].load(std::memory_order_relaxed) : 0.0f;
}

bool Bridge::activate(double sample_rate, std::uint32_t max_frames) {
    any_dirty_.store(false, std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < count_; ++i) {
        if (dirty_[i].exchange(false, std::memory_order_acq_rel)) {
            processor_->set_param(i, values_[i].load(std::memory_order_relaxed));
        }
    }
    // A host that does not say how long its blocks are still gets a
    // working instance: the Processor splits longer blocks itself, so this
    // only sets the size of its internal chunks.
    if (max_frames == 0) max_frames = 1024;

    bool same = prepared_ && sample_rate == prepared_rate_ && max_frames == prepared_max_frames_;
    const Descriptor& d = descriptor();
    for (std::uint32_t i = 0; same && i < count_; ++i) {
        if (d.params[i].reconfigures && processor_->param(i) != prepared_values_[i]) same = false;
    }
    if (same) {
        // What prepare() would have done, without its allocations: every
        // value where it is headed (a jump; a value held for a declick
        // lands now), and no signal state carried over. One difference: a
        // declick fade under way finishes its remaining few milliseconds,
        // where prepare() would have cut it.
        for (std::uint32_t i = 0; i < count_; ++i) processor_->set_param(i, processor_->param(i));
        processor_->reset();
        first_block_ = true;
        return true;
    }

    prepared_ = processor_->prepare(sample_rate, max_frames, channels_);
    latency_ = prepared_ ? processor_->latency() : 0;
    prepared_rate_ = sample_rate;
    prepared_max_frames_ = max_frames;
    for (std::uint32_t i = 0; i < count_; ++i) prepared_values_[i] = processor_->param(i);
    first_block_ = true;
    return prepared_;
}

void Bridge::run(const float* const* in, float* const* out, std::uint32_t frames,
                 const Transport& transport) noexcept {
    if (frames == 0) return;
    if (!prepared_) {
        silence(out, channels_, frames);
        return;
    }
    std::uint32_t n = 0;
    if (any_dirty_.exchange(false, std::memory_order_acq_rel)) {
        for (std::uint32_t i = 0; i < count_; ++i) {
            if (!dirty_[i].exchange(false, std::memory_order_acq_rel)) continue;
            const float v = values_[i].load(std::memory_order_relaxed);
            // Before any audio has been heard there is nothing to smooth
            // from: the value is where the instance starts.
            if (first_block_) processor_->set_param(i, v);
            else events_[n++] = ParamEvent{0, i, v};
        }
    }
    first_block_ = false;
    const AudioBuffers io{in, out, nullptr, channels_, 0, frames};
    processor_->process(io, events_.get(), n, transport);
}

}  // namespace sonora::native::format
