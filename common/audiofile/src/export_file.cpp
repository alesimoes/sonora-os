// SPDX-License-Identifier: GPL-3.0-or-later
#include "export_file.h"

#include "flac_bits.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

namespace sonora {

namespace {

constexpr uint32_t kBextSize = 602;
constexpr uint32_t kDs64Body = 28;   // riffSize, dataSize, sampleCount (8 each), tableLength (4)
constexpr std::size_t kDs64At = 12;  // after "RIFF", its size and "WAVE"

// --- little helpers -----------------------------------------------------------

void put_le(std::vector<uint8_t>& b, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void put_be(std::vector<uint8_t>& b, uint64_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void put_tag(std::vector<uint8_t>& b, const char* t) { b.insert(b.end(), t, t + 4); }

// std::localtime shares its storage across callers; one lock for ours.
bool local_time(std::tm& out) {
    static std::mutex mutex;
    const std::time_t now = std::time(nullptr);
    if (now == static_cast<std::time_t>(-1)) return false;
    const std::lock_guard<std::mutex> lock(mutex);
    const std::tm* t = std::localtime(&now);
    if (t == nullptr) return false;
    out = *t;
    return true;
}

}  // namespace

OutFile::OutFile(uint64_t size_limit) : size_limit_(size_limit) {}

OutFile::~OutFile() { abort(); }

bool OutFile::open(const std::string& path, ExportContainer c, int bits, uint32_t rate, uint16_t ch,
                   const ExportMeta& meta, std::string& error) {
    path_ = path;
    c_ = c;
    bits_ = bits;
    rate_ = rate;
    ch_ = ch;
    file_ = std::fopen(path.c_str(), "wb");
    if (file_ == nullptr) {
        error = "cannot create " + path + ": " + std::strerror(errno);
        return false;
    }
    std::vector<uint8_t> h;
    if (c == ExportContainer::Bwf || c == ExportContainer::Wav) {
        put_tag(h, "RIFF");
        put_le(h, 0, 4);
        put_tag(h, "WAVE");
        // Room for ds64: close() turns this into the RF64 size table when the
        // finished file is too big for 32-bit sizes, and leaves it as padding
        // otherwise. Always written, so nothing is buffered or moved later.
        put_tag(h, "JUNK");
        put_le(h, kDs64Body, 4);
        h.resize(h.size() + kDs64Body, 0);
        if (c == ExportContainer::Bwf) append_bext(h, meta);
        put_tag(h, "fmt ");
        put_le(h, 16, 4);
        put_le(h, bits == 32 ? 3 : 1, 2);
        put_le(h, ch, 2);
        put_le(h, rate, 4);
        put_le(h, uint64_t{rate} * ch * static_cast<uint64_t>(bits / 8), 4);
        put_le(h, static_cast<uint64_t>(ch * (bits / 8)), 2);
        put_le(h, static_cast<uint64_t>(bits), 2);
        put_tag(h, "data");
        data_size_at_ = h.size();
        put_le(h, 0, 4);
    } else if (c == ExportContainer::Aiff) {
        put_tag(h, "FORM");
        put_be(h, 0, 4);
        put_tag(h, "AIFF");
        put_tag(h, "COMM");
        put_be(h, 18, 4);
        put_be(h, ch, 2);
        frames_at_ = h.size();
        put_be(h, 0, 4);
        put_be(h, static_cast<uint64_t>(bits), 2);
        put_extended(h, rate);
        put_tag(h, "SSND");
        data_size_at_ = h.size();
        put_be(h, 8, 4);
        put_be(h, 0, 4);   // offset
        put_be(h, 0, 4);   // block size
    } else {
        put_tag(h, "fLaC");
        h.push_back(0x80);   // last metadata block, STREAMINFO
        put_be(h, 34, 3);
        h.resize(h.size() + 34, 0);   // patched in close()
        block_.assign(static_cast<std::size_t>(ch) * kBlockFrames, 0);
    }
    return put(h, error);
}

bool OutFile::write_int(const int32_t* il, std::size_t frames, std::string& error) {
    if (c_ == ExportContainer::Flac) return flac_add(il, frames, error);
    std::vector<uint8_t> b;
    const int bytes = bits_ / 8;
    b.reserve(frames * ch_ * static_cast<std::size_t>(bytes));
    for (std::size_t i = 0; i < frames * ch_; ++i) {
        const uint32_t v = static_cast<uint32_t>(il[i]);
        if (c_ == ExportContainer::Aiff) put_be(b, v, bytes);
        else put_le(b, v, bytes);
    }
    data_bytes_ += b.size();
    frames_ += frames;
    return put(b, error);
}

bool OutFile::write_float(const float* il, std::size_t frames, std::string& error) {
    std::vector<uint8_t> b;
    b.reserve(frames * ch_ * 4);
    for (std::size_t i = 0; i < frames * ch_; ++i) {
        uint32_t v;
        std::memcpy(&v, &il[i], 4);
        put_le(b, v, 4);
    }
    data_bytes_ += b.size();
    frames_ += frames;
    return put(b, error);
}

bool OutFile::close(std::string& error) {
    if (file_ == nullptr) return true;
    bool ok = true;
    if (c_ == ExportContainer::Flac) {
        ok = flac_finish(error);
    } else {
        std::vector<uint8_t> pad;
        if (data_bytes_ % 2 != 0) pad.push_back(0);
        ok = put(pad, error);
        const uint64_t total = written_;
        const bool over = total > size_limit_;
        if (ok && over && c_ == ExportContainer::Aiff) {
            error = path_ + " is over 4 GiB, which a WAV cannot hold";
            ok = false;
        }
        if (ok) {
            if (c_ == ExportContainer::Aiff) {
                patch(4, total - 8, 4, true);
                patch(frames_at_, frames_, 4, true);
                patch(data_size_at_, data_bytes_ + 8, 4, true);
            } else if (over) {
                patch_rf64(total);
            } else {
                patch(4, total - 8, 4, false);
                patch(data_size_at_, data_bytes_, 4, false);
            }
        }
    }
    if (std::fflush(file_) != 0 && ok) {
        error = "cannot write " + path_ + ": " + std::strerror(errno);
        ok = false;
    }
    if (std::fclose(file_) != 0 && ok) {
        error = "cannot write " + path_ + ": " + std::strerror(errno);
        ok = false;
    }
    file_ = nullptr;
    return ok;
}

void OutFile::abort() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

bool OutFile::put(const std::vector<uint8_t>& b, std::string& error) {
    if (!b.empty() && std::fwrite(b.data(), 1, b.size(), file_) != b.size()) {
        error = "cannot write " + path_ + ": " + std::strerror(errno);
        return false;
    }
    written_ += b.size();
    return true;
}

void OutFile::patch(std::size_t at, uint64_t v, int bytes, bool big) {
    std::vector<uint8_t> b;
    if (big) put_be(b, v, bytes);
    else put_le(b, v, bytes);
    patch_bytes(at, b);
}

// Only called from close(), after the last sample: no position to restore.
void OutFile::patch_bytes(std::size_t at, const std::vector<uint8_t>& b) {
    std::fseek(file_, static_cast<long>(at), SEEK_SET);
    std::fwrite(b.data(), 1, b.size(), file_);
}

// EBU Tech 3306: RF64 + ds64 in place of RIFF + JUNK; the 32-bit sizes that
// would not fit say 0xFFFFFFFF and ds64 holds the real 64-bit ones.
void OutFile::patch_rf64(uint64_t total) {
    std::vector<uint8_t> h;
    put_tag(h, "RF64");
    put_le(h, 0xFFFFFFFFu, 4);
    patch_bytes(0, h);
    h.clear();
    put_tag(h, "ds64");
    put_le(h, kDs64Body, 4);
    put_le(h, total - 8, 8);
    put_le(h, data_bytes_, 8);
    put_le(h, frames_, 8);
    put_le(h, 0, 4);   // tableLength: no chunk sizes beyond data
    patch_bytes(kDs64At, h);
    patch(data_size_at_, 0xFFFFFFFFu, 4, false);
}

// IEEE 754 extended, 80 bit, for AIFF's COMM sample rate.
void OutFile::put_extended(std::vector<uint8_t>& b, uint32_t rate) {
    int e = 0;
    while ((uint64_t{rate} >> (e + 1)) != 0) ++e;
    put_be(b, static_cast<uint64_t>(16383 + e), 2);
    put_be(b, uint64_t{rate} << (63 - e), 8);
}

// EBU Tech 3285 v2: the origin of the file on the timeline, the loudness
// the file was measured at, and when it was written.
void OutFile::append_bext(std::vector<uint8_t>& h, const ExportMeta& meta) {
    put_tag(h, "bext");
    put_le(h, kBextSize, 4);
    const std::size_t base = h.size();
    h.resize(base + kBextSize, 0);
    uint8_t* b = h.data() + base;
    std::memcpy(b, meta.description.data(), std::min<std::size_t>(meta.description.size(), 256));
    std::memcpy(b + 256, "Sonora Engine", 13);
    std::tm now{};
    if (local_time(now) && now.tm_year >= 0 && now.tm_year < 8100) {
        char text[20];
        std::snprintf(text, sizeof text, "%04d-%02d-%02d", now.tm_year + 1900, now.tm_mon + 1,
                      now.tm_mday);
        std::memcpy(b + 320, text, 10);
        std::snprintf(text, sizeof text, "%02d:%02d:%02d", now.tm_hour, now.tm_min,
                      std::min(now.tm_sec, 59));
        std::memcpy(b + 330, text, 8);
    }
    for (int i = 0; i < 8; ++i) b[338 + i] = static_cast<uint8_t>(meta.time_reference >> (8 * i));
    b[346] = 2;   // BWF version 2: the loudness fields below
    // 0x7FFF is "not given" for each; the value is in hundredths of LU.
    const auto field = [&](std::size_t at, int v) {
        b[at] = static_cast<uint8_t>(v & 0xFF);
        b[at + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    };
    field(412, meta.loudness_lufs > -70.0
                   ? static_cast<int>(std::lround(meta.loudness_lufs * 100.0))
                   : 0x7FFF);
    field(414, 0x7FFF);
    field(416, 0x7FFF);
    field(418, 0x7FFF);
    field(420, 0x7FFF);
}

// --- FLAC ---

bool OutFile::flac_add(const int32_t* il, std::size_t frames, std::string& error) {
    for (std::size_t i = 0; i < frames; ++i) {
        for (uint16_t c = 0; c < ch_; ++c) {
            const int32_t v = il[i * ch_ + c];
            block_[c * kBlockFrames + filled_] = v;
            const uint32_t u = static_cast<uint32_t>(v);
            md5_bytes_.push_back(static_cast<uint8_t>(u));
            md5_bytes_.push_back(static_cast<uint8_t>(u >> 8));
            if (bits_ == 24) md5_bytes_.push_back(static_cast<uint8_t>(u >> 16));
        }
        if (++filled_ == kBlockFrames && !flac_frame(error)) return false;
    }
    md5_.update(md5_bytes_.data(), md5_bytes_.size());
    md5_bytes_.clear();
    frames_ += frames;
    return true;
}

bool OutFile::flac_frame(std::string& error) {
    const std::size_t n = filled_;
    filled_ = 0;
    if (n == 0) return true;
    std::vector<uint8_t> f;
    f.push_back(0xFF);
    f.push_back(0xF8);   // fixed block size
    const bool full = n == kBlockFrames;
    f.push_back(static_cast<uint8_t>(((full ? 12 : 7) << 4) | 0));   // 4096, or 16-bit follows; rate in STREAMINFO
    f.push_back(static_cast<uint8_t>(((ch_ - 1) << 4) | ((bits_ == 16 ? 4 : 6) << 1)));
    put_utf8_number(f, frame_number_++);
    if (!full) put_be(f, n - 1, 2);
    f.push_back(crc8(f.data(), f.size()));
    BitWriter bw(f);
    for (uint16_t c = 0; c < ch_; ++c) encode_subframe(bw, &block_[c * kBlockFrames], n, bits_);
    bw.align();
    const uint16_t crc = crc16(f.data(), f.size());
    f.push_back(static_cast<uint8_t>(crc >> 8));
    f.push_back(static_cast<uint8_t>(crc));
    min_frame_ = std::min<uint64_t>(min_frame_, f.size());
    max_frame_ = std::max<uint64_t>(max_frame_, f.size());
    // The stream's block size is the full one: a short last block must
    // not make it variable-size, which a reference decoder numbers
    // differently (and then warns about).
    if (full || frame_number_ == 1) {
        min_block_ = std::min<uint64_t>(min_block_, n);
        max_block_ = std::max<uint64_t>(max_block_, n);
    }
    return put(f, error);
}

bool OutFile::flac_finish(std::string& error) {
    if (!flac_frame(error)) return false;
    uint8_t digest[16];
    md5_.finish(digest);
    std::vector<uint8_t> si;
    BitWriter bw(si);
    const uint64_t none = 0;
    bw.put(frame_number_ == 0 ? 4096 : min_block_, 16);
    bw.put(frame_number_ == 0 ? 4096 : max_block_, 16);
    bw.put(frame_number_ == 0 ? none : min_frame_, 24);
    bw.put(frame_number_ == 0 ? none : max_frame_, 24);
    bw.put(rate_, 20);
    bw.put(static_cast<uint64_t>(ch_ - 1), 3);
    bw.put(static_cast<uint64_t>(bits_ - 1), 5);
    bw.put(frames_ >> 32, 4);
    bw.put(frames_ & 0xFFFFFFFFu, 32);
    bw.align();
    si.insert(si.end(), digest, digest + 16);
    std::fseek(file_, 8, SEEK_SET);
    if (std::fwrite(si.data(), 1, si.size(), file_) != si.size()) {
        error = "cannot write " + path_ + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

}  // namespace sonora
