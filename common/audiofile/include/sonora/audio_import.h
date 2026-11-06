// SPDX-License-Identifier: GPL-3.0-or-later
//
// Import: a WAV file of any sample rate becomes a BWF file at the session's
// rate (ui.editing.import). A session plays every source at its own rate
// with no conversion on the audio thread, so the conversion happens here,
// once, off every real-time path, and what the session holds is the
// converted copy. The original is never touched.
//
// Free of Qt and of the model, so a test can run it without a display.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "sonora/wav_writer.h"

namespace sonora {

struct AudioInfo {
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    int64_t frames = 0;
    bool floating = false;  // 32-bit float; otherwise 16 or 24-bit integer
    uint64_t time_reference = 0;
};

// Reads only the header. False, `why` saying what is wrong with the file
// (not WAV, an encoding the reader does not read, no audio).
bool probe_audio(const std::string& path, AudioInfo& out, std::string& why);

// Streaming sample-rate converter: a Kaiser-windowed sinc, 32 zero crossings
// on each side at the lower of the two rates, so a downward conversion low
// passes below the new Nyquist instead of aliasing. The kernel is tabulated
// at 1024 phases and interpolated linearly.
// Allocates; never run it on the audio thread.
class Resampler {
public:
    Resampler(uint32_t in_rate, uint32_t out_rate, uint16_t channels);

    // Frames the whole input of `in_frames` converts to.
    int64_t output_frames(int64_t in_frames) const;

    // Feeds `frames` interleaved frames and appends the output frames that
    // are complete to `out`. Call finish() after the last block.
    void push(const float* interleaved, std::size_t frames, std::vector<float>& out);
    // The tail: the frames that needed input beyond the end, which is zero.
    void finish(int64_t in_frames, std::vector<float>& out);

private:
    void produce(std::vector<float>& out, int64_t limit_in, int64_t total_out);

    uint32_t in_rate_, out_rate_;
    uint16_t channels_;
    double scale_;            // >= 1: how much wider the kernel is than at equal rates
    int half_;                // taps on each side of the centre
    std::vector<float> table_;
    std::vector<float> buf_;  // interleaved input from frame `buf_start_`
    int64_t buf_start_ = 0;
    int64_t in_total_ = 0;    // input frames received
    int64_t next_out_ = 0;
};

struct ImportResult {
    uint16_t channels = 0;
    int64_t frames = 0;  // at the session's rate
    bool floating = false;
    int64_t time_reference = 0;  // at the session's rate
    bool resampled = false;
};

// Reads `src`, converts to `rate`, writes `dest` (which must not exist) as a
// BWF at 24 bits, or 32-bit float when the source is. `progress` runs 0 to
// 1000 while it works; `cancel`, once true, stops it and removes `dest`.
// False with `why` on any failure, `dest` removed; `why` is empty on cancel.
bool import_audio(const std::string& src, const std::string& dest, uint32_t rate,
                  std::atomic<int>* progress, const std::atomic<bool>* cancel,
                  ImportResult& out, std::string& why);

}  // namespace sonora
