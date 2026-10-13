// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

namespace sonora {

// Broadcast Wave (BWF) writer. Writes a bext chunk so the file carries its
// own timecode origin, which is what every other DAW reads on import.
//
// All writes happen on the disk thread, never on the audio callback.
class WavWriter {
public:
    enum class Format { Int24, Float32 };

    // Fills `out` with the local wall-clock time, or returns false if there
    // is none to give.
    using LocalClock = bool (*)(std::tm& out);

    // The largest RIFF ckSize a plain WAV can state. Until RF64 lands, a take
    // stops growing here rather than writing a header that has wrapped.
    static constexpr uint64_t kRiffSizeLimit = 0xFFFFFFFFu;

    WavWriter() = default;
    ~WavWriter();

    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    // time_reference is the sample position of the first frame relative to
    // midnight, as BWF defines it. Fails, leaving it untouched, if `path`
    // already exists; errno is then EEXIST. Fails too, leaving no file, if
    // the header cannot be written to disk. On any failure errno says why.
    bool open(const std::string& path, uint32_t sample_rate, uint16_t channels,
              Format format, uint64_t time_reference);

    // Writes interleaved float frames, converting to the on-disk format.
    // Returns false and marks the take failed when the frames cannot all be
    // written: the file then ends at the last whole frame that was, and
    // every later call writes nothing until the next open().
    bool write(const float* interleaved, std::size_t frames);

    // Patches the RIFF/data sizes, pads an odd data chunk, and closes. Safe
    // to call twice.
    bool close();

    // Replaces the time_reference given to open(); close() writes it. For an
    // origin known only once the first frame has been recorded, which is
    // after the file had to be opened. Same thread as write() and close().
    void set_time_reference(uint64_t time_reference) {
        time_reference_ = time_reference;
        patch_time_reference_ = true;
    }

    // Where OriginationDate and OriginationTime come from. The default reads
    // the system's local time. Set before open().
    void set_clock(LocalClock clock) { clock_ = clock; }

    // Tests lower this to reach the limit without writing 4 GiB; it cannot
    // be raised past kRiffSizeLimit. Set before open().
    void set_riff_size_limit(uint64_t bytes);

    // Whether this take has failed, and why. Safe to ask from another thread
    // while the take runs; failure() is settled once failed() is true, and
    // stays until the next open().
    bool failed() const { return failed_.load(std::memory_order_acquire); }
    const std::string& failure() const { return failure_; }

    bool is_open() const { return file_ != nullptr; }
    uint64_t frames_written() const { return frames_written_; }
    const std::string& path() const { return path_; }

private:
    bool write_header(uint32_t sample_rate, uint16_t channels,
                      uint64_t time_reference);
    void fail(const char* why);
    uint32_t bytes_per_frame() const;

    std::FILE* file_ = nullptr;
    std::string path_;
    Format format_ = Format::Int24;
    uint16_t channels_ = 0;
    uint64_t frames_written_ = 0;
    long data_size_offset_ = 0;
    long riff_size_offset_ = 0;
    long time_reference_offset_ = 0;
    uint64_t time_reference_ = 0;
    bool patch_time_reference_ = false;
    LocalClock clock_ = nullptr;
    uint64_t riff_size_limit_ = kRiffSizeLimit;
    std::string failure_;
    std::atomic<bool> failed_{false};
};

}  // namespace sonora
