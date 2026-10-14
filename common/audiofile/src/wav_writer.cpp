// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/wav_writer.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <vector>

#include "sonora/platform/file.h"

namespace sonora {
namespace {

void put_u16(std::FILE* f, uint16_t v) { std::fwrite(&v, 2, 1, f); }
void put_u32(std::FILE* f, uint32_t v) { std::fwrite(&v, 4, 1, f); }
void put_tag(std::FILE* f, const char tag[4]) { std::fwrite(tag, 1, 4, f); }

int32_t float_to_int24(float s) {
    // A NaN would make every comparison below false and leave lrintf with
    // undefined behaviour, writing garbage into the file. One bad sample is
    // worth a branch to contain.
    if (std::isnan(s)) return 0;

    // Scale by 2^23, not by 2^23-1: -1.0 then lands exactly on the most
    // negative code and the whole 24-bit range is used.
    const float scaled = s * 8388608.0f;

    // Clamping happens on the scaled value rather than on the input, because
    // +1.0 scales to 8388608, which is one past the largest code. Clamping
    // the input to 1.0 first would not have prevented that.
    if (scaled >= 8388607.0f) return 8388607;
    if (scaled <= -8388608.0f) return -8388608;
    return static_cast<int32_t>(std::lrintf(scaled));
}

constexpr uint32_t kBextSize = 602;  // fixed-size BWF v0 bext chunk
constexpr uint32_t kFmtSize = 16;

// RIFF ckSize before the first sample: "WAVE" and the bext, fmt and data
// chunks' headers and fixed payloads.
constexpr uint64_t kRiffOverhead = 4 + (8 + kBextSize) + (8 + kFmtSize) + 8;

constexpr char kLimitReason[] = "take reached the 4 GB WAV limit";

// std::localtime returns a pointer to storage shared by every caller in the
// process, and standard C++17 has no reentrant form (localtime_r is POSIX,
// localtime_s is Windows, so neither is allowed here, ADR-0008). The lock
// covers every call this writer makes; nothing else in the engine calls it.
bool system_local_time(std::tm& out) {
    static std::mutex mutex;
    const std::time_t now = std::time(nullptr);
    if (now == static_cast<std::time_t>(-1)) return false;
    const std::lock_guard<std::mutex> lock(mutex);
    const std::tm* local = std::localtime(&now);
    if (!local) return false;
    out = *local;
    return true;
}

// Zero-padded decimal, exactly `width` characters. Hand-rolled rather than
// snprintf so nothing is written past the field and no terminator is needed.
bool put_digits(char* dst, int value, int lo, int hi, int width) {
    if (value < lo || value > hi) return false;
    for (int i = width - 1; i >= 0; --i) {
        dst[i] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
    return true;
}

// OriginationDate "yyyy-mm-dd" and OriginationTime "hh:mm:ss" (EBU Tech 3285
// v2 §2.3). The spec gives no time zone; it means local time, the wall clock
// of the place the recording was made, which is also what Pro Tools and
// other recorders write. A time the fields cannot hold leaves them zero, as
// a missing clock does: an empty date is better than a wrong one.
void put_origination(char* field, const std::tm& t) {
    char text[18];
    const bool ok =
        put_digits(text + 0, t.tm_year + 1900, 0, 9999, 4) &&
        put_digits(text + 5, t.tm_mon + 1, 1, 12, 2) &&
        put_digits(text + 8, t.tm_mday, 1, 31, 2) &&
        put_digits(text + 10, t.tm_hour, 0, 23, 2) &&
        put_digits(text + 13, t.tm_min, 0, 59, 2) &&
        // 60 is a leap second, which std::tm allows.
        put_digits(text + 16, t.tm_sec, 0, 60, 2);
    if (!ok) return;
    text[4] = '-';
    text[7] = '-';
    text[12] = ':';
    text[15] = ':';
    std::memcpy(field, text, sizeof text);
}

bool put_frames(std::FILE* f, WavWriter::Format format, const float* in,
                std::size_t samples) {
    if (format == WavWriter::Format::Float32) {
        return std::fwrite(in, sizeof(float), samples, f) == samples;
    }
    std::vector<uint8_t> packed(samples * 3);
    for (std::size_t i = 0; i < samples; ++i) {
        const int32_t v = float_to_int24(in[i]);
        packed[i * 3 + 0] = static_cast<uint8_t>(v & 0xFF);
        packed[i * 3 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        packed[i * 3 + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    }
    return std::fwrite(packed.data(), 1, packed.size(), f) == packed.size();
}

}  // namespace

WavWriter::~WavWriter() { close(); }

void WavWriter::set_riff_size_limit(uint64_t bytes) {
    riff_size_limit_ = std::min(bytes, kRiffSizeLimit);
}

uint32_t WavWriter::bytes_per_frame() const {
    return channels_ * ((format_ == Format::Int24) ? 3u : 4u);
}

void WavWriter::fail(const char* why) {
    failure_ = why;
    failed_.store(true, std::memory_order_release);
}

bool WavWriter::open(const std::string& path, uint32_t sample_rate,
                     uint16_t channels, Format format,
                     uint64_t time_reference) {
    if (file_) return false;
    // Exclusive: if the name is taken, fail with EEXIST rather than
    // truncate. Choosing a free take number is the session's job; this is
    // what stops a mistake there from destroying a recording. Not inherited
    // by the interface the engine starts while a take is open.
    file_ = platform::create_exclusive(path);
    if (!file_) return false;
    path_ = path;
    format_ = format;
    channels_ = channels;
    frames_written_ = 0;
    patch_time_reference_ = false;
    folder_synced_ = false;
    failure_.clear();
    failed_.store(false, std::memory_order_release);
    if (!write_header(sample_rate, channels, time_reference)) {
        // The caller reports errno as the reason, and the cleanup below is
        // allowed to change it.
        const int err = errno;
        std::fclose(file_);
        file_ = nullptr;
        // Created by the call above, so it is ours to remove, and a header
        // with no audio behind it would only hold the take number.
        std::remove(path.c_str());
        errno = err;
        return false;
    }
    return true;
}

bool WavWriter::write_header(uint32_t sample_rate, uint16_t channels,
                             uint64_t time_reference) {
    const uint16_t bits = (format_ == Format::Int24) ? 24 : 32;
    const uint16_t audio_format = (format_ == Format::Int24) ? 1 : 3;  // PCM/IEEE float
    const uint16_t block_align = channels * (bits / 8);

    put_tag(file_, "RIFF");
    riff_size_offset_ = std::ftell(file_);
    put_u32(file_, 0);  // patched in close()
    put_tag(file_, "WAVE");

    // --- bext ---
    put_tag(file_, "bext");
    put_u32(file_, kBextSize);
    time_reference_offset_ = std::ftell(file_) + 338;
    std::vector<char> bext(kBextSize, 0);
    const char* originator = "Sonora Engine";
    std::memcpy(bext.data() + 256, originator, std::strlen(originator));
    std::tm now{};
    if ((clock_ ? clock_ : &system_local_time)(now)) {
        put_origination(bext.data() + 320, now);
    }
    const uint32_t tr_low = static_cast<uint32_t>(time_reference & 0xFFFFFFFFu);
    const uint32_t tr_high = static_cast<uint32_t>(time_reference >> 32);
    std::memcpy(bext.data() + 338, &tr_low, 4);
    std::memcpy(bext.data() + 342, &tr_high, 4);
    const uint16_t bwf_version = 0;
    std::memcpy(bext.data() + 346, &bwf_version, 2);
    std::fwrite(bext.data(), 1, bext.size(), file_);

    // --- fmt ---
    put_tag(file_, "fmt ");
    put_u32(file_, kFmtSize);
    put_u16(file_, audio_format);
    put_u16(file_, channels);
    put_u32(file_, sample_rate);
    put_u32(file_, sample_rate * block_align);  // byte rate
    put_u16(file_, block_align);
    put_u16(file_, bits);

    // --- data ---
    put_tag(file_, "data");
    data_size_offset_ = std::ftell(file_);
    put_u32(file_, 0);  // patched in close()
    // Flushed so a disk that cannot take even the header fails here, where
    // start_recording() can refuse the take and say why, and not in the disk
    // thread once the operator believes it is recording.
    return std::fflush(file_) == 0 && std::ferror(file_) == 0;
}

bool WavWriter::write(const float* interleaved, std::size_t frames) {
    if (!file_ || failed()) return false;
    if (frames == 0) return true;

    // The most frames the 32-bit sizes can describe, counting the pad byte
    // close() adds after odd-sized data. Past that the header would wrap
    // and readers would see a short or garbled file, so the take stops at
    // the last whole frame that fits.
    const uint64_t bpf = bytes_per_frame();
    const uint64_t room =
        riff_size_limit_ > kRiffOverhead ? riff_size_limit_ - kRiffOverhead : 0;
    uint64_t max_frames = room / bpf;
    if ((max_frames * bpf) % 2 == 1 && max_frames * bpf + 1 > room) --max_frames;
    const uint64_t left =
        max_frames > frames_written_ ? max_frames - frames_written_ : 0;
    const std::size_t fit =
        static_cast<std::size_t>(std::min<uint64_t>(frames, left));

    if (fit > 0) {
        if (!put_frames(file_, format_, interleaved, fit * channels_)) {
            fail(std::strerror(errno));
            return false;
        }
        frames_written_ += fit;
    }
    if (fit < frames) {
        fail(kLimitReason);
        return false;
    }
    return true;
}

void WavWriter::patch_sizes(uint64_t riff_bytes, uint64_t data_bytes) {
    // write() keeps both within riff_size_limit_, at most 2^32 - 1, so
    // neither cast wraps.
    std::fseek(file_, data_size_offset_, SEEK_SET);
    put_u32(file_, static_cast<uint32_t>(data_bytes));
    std::fseek(file_, riff_size_offset_, SEEK_SET);
    put_u32(file_, static_cast<uint32_t>(riff_bytes));
    if (patch_time_reference_) {
        std::fseek(file_, time_reference_offset_, SEEK_SET);
        put_u32(file_, static_cast<uint32_t>(time_reference_ & 0xFFFFFFFFu));
        put_u32(file_, static_cast<uint32_t>(time_reference_ >> 32));
    }
}

bool WavWriter::checkpoint() {
    if (!file_ || failed()) return false;
    const uint64_t data_bytes = frames_written_ * bytes_per_frame();
    patch_sizes(kRiffOverhead + data_bytes, data_bytes);
    // The next write() appends; it must not land on the header.
    std::fseek(file_, 0, SEEK_END);
    if (std::ferror(file_) != 0) {
        fail(std::strerror(errno));
        return false;
    }
    // One sync for the header and the audio together. Under a power cut
    // mid-sync the header may reach the medium without all the audio it
    // counts; recover_take() then reports the file short, and nothing is
    // invented. For kill -9 the page cache holds both, in order.
    if (!(sync_ ? sync_ : &platform::sync_data)(file_)) {
        // Linux clears the error a failed fdatasync reports, so a retry
        // would succeed without the data: the take is lost from here.
        if (errno != ENOSYS) {
            fail(std::strerror(errno));
            return false;
        }
    }
    if (!folder_synced_) {
        const std::filesystem::path folder = std::filesystem::path(path_).parent_path();
        // Retried at the next checkpoint if it fails: the file's data is
        // safe meanwhile, only its name is not yet.
        folder_synced_ = platform::sync_directory(folder.empty() ? "." : folder.string());
    }
    return true;
}

bool WavWriter::flush() {
    if (!file_ || failed()) return false;
    if (std::fflush(file_) != 0) {
        fail(std::strerror(errno));
        return false;
    }
    return true;
}

bool WavWriter::close() {
    if (!file_) return true;
    const uint64_t data_bytes = frames_written_ * bytes_per_frame();
    // RIFF chunks start on even offsets, so odd-sized data is followed by a
    // zero byte that the data chunk's size does not count but the RIFF size
    // does. Mono 24-bit with an odd frame count is the case that needs it.
    const uint64_t pad = data_bytes % 2;
    const uint64_t riff_bytes = kRiffOverhead + data_bytes + pad;

    if (pad && !failed()) {
        std::fseek(file_, 0, SEEK_END);
        std::fputc(0, file_);
    }
    patch_sizes(riff_bytes, data_bytes);

    bool ok = std::ferror(file_) == 0;
    ok = std::fclose(file_) == 0 && ok;
    file_ = nullptr;
    if (failed()) {
        // A failed write can leave part of a frame past the last whole one,
        // where the pad byte belongs. Cutting the file back to the audio
        // and then growing it by the pad drops that tail and makes the pad
        // zero, since a file grown by resize_file reads as zeros.
        const uint64_t audio_end = 8 + riff_bytes - pad;
        std::error_code ec;
        std::filesystem::resize_file(path_, audio_end, ec);
        if (!ec && pad) std::filesystem::resize_file(path_, audio_end + pad, ec);
    }
    if (!ok && !failed()) fail("the file could not be finished on disk");
    return ok;
}

}  // namespace sonora
