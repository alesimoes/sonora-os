// SPDX-License-Identifier: GPL-3.0-or-later
//
// The amp's one class, split by thread: amp.cpp is the audio thread
// (render, reset, the hand-over), loader.cpp the control threads (loads,
// prepare, the file slot, the class-E timing).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "chain.h"
#include "sonora/native/amp.h"
#include "sonora/native/files.h"

namespace sonora::native::amp {

// One of the two hand-over slots. A null chain is "no capture".
struct Slot {
    std::unique_ptr<ModelChain> chain;
    float trim = 1.0f;  // the normalisation gain, applied when "normalise" is on
};

class AmpImpl final : public Amp, public FileSlots {
public:
    AmpImpl();

    // Control threads (loader.cpp).
    LoadResult load_capture(CaptureSource source, std::string& message) override;
    LoadResult load_nam(const ampcab::NamModel& model, std::string& message) override;
    LoadResult clear_capture() override;
    std::string notice() const override;
    Status status() const override;
    void set_budget(Budget budget) override { budget_ = budget; }

    FileSlots* files() noexcept override { return this; }
    const FileSlots* files() const noexcept override { return this; }
    std::uint32_t slot_count() const noexcept override { return 1; }
    const FileSlotInfo& slot(std::uint32_t) const noexcept override;
    bool load(std::uint32_t index, const std::string& path, std::string& error) override;
    std::string path(std::uint32_t) const override;

protected:
    bool on_prepare() override;
    void on_reset() noexcept override;
    void render(const float* const* in, float* const* out, const float* const* sidechain,
                std::uint32_t sidechain_channels, std::uint32_t frames) noexcept override;

private:
    enum class Phase { Steady, Out, Hold, In };

    // loader.cpp; load_mutex_ held.
    bool build_slot(const CaptureSource& src, double session_rate, std::uint32_t max_frames, Slot& slot,
                    Status& status, std::string& message);
    std::uint32_t default_latency() const;
    void size_buffers(std::uint32_t latency);

    // amp.cpp; audio thread.
    std::uint32_t phase_len() const noexcept { return phase_ == Phase::Hold ? hold_len_ : fade_len_; }
    double phase_gain(std::uint32_t i) const noexcept;
    void advance_phase(std::uint32_t frames) noexcept;

    Budget budget_;

    mutable std::mutex file_mutex_;  // control threads; held across a whole file load
    std::string path_;

    mutable std::mutex load_mutex_;  // control threads only; never taken by render()
    bool prepared_ = false;
    bool has_source_ = false;
    CaptureSource source_;  // the capture prepare() rebuilds
    Status status_;
    std::string notice_;

    Slot slots_[2];
    std::atomic<Slot*> pending_{nullptr};  // loader -> audio
    std::atomic<Slot*> idle_{nullptr};     // audio -> loader
    Slot* cur_ = nullptr;                  // audio thread only
    Slot* incoming_ = nullptr;

    // The dry path, delayed by the latency so the mix lines up with the wet.
    std::vector<float> dry_;  // [channel][dry_len_]
    std::uint32_t dry_len_ = 1, dry_pos_ = 0;
    std::vector<float> mono_, wet_;  // [max_frames]

    Phase phase_ = Phase::Steady;
    std::uint32_t fpos_ = 0, fade_len_ = 16, hold_len_ = 16;
    std::vector<double> fade_;  // raised cosine, 1 -> 0
};

}  // namespace sonora::native::amp
