// SPDX-License-Identifier: GPL-3.0-or-later
#include "export_file.h"

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

// --- bit writer for FLAC -------------------------------------------------------

class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}
    void put(uint64_t v, int n) {   // n <= 32
        if (n <= 0) return;
        acc_ = (acc_ << n) | (v & ((uint64_t{1} << n) - 1));
        nbits_ += n;
        while (nbits_ >= 8) {
            out_.push_back(static_cast<uint8_t>(acc_ >> (nbits_ - 8)));
            nbits_ -= 8;
        }
        acc_ &= (uint64_t{1} << nbits_) - 1;
    }
    void unary_zeroes_then_one(uint64_t q) {
        while (q > 0) {
            const int n = static_cast<int>(std::min<uint64_t>(q, 24));
            put(0, n);
            q -= static_cast<uint64_t>(n);
        }
        put(1, 1);
    }
    void align() {
        if (nbits_ > 0) put(0, 8 - nbits_);
    }

private:
    std::vector<uint8_t>& out_;
    uint64_t acc_ = 0;
    int nbits_ = 0;
};

uint8_t crc8(const uint8_t* p, std::size_t n) {
    uint8_t c = 0;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int b = 0; b < 8; ++b) c = static_cast<uint8_t>((c & 0x80) ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}

uint16_t crc16(const uint8_t* p, std::size_t n) {
    static uint16_t table[256];
    static bool made = false;
    static std::mutex m;
    {
        const std::lock_guard<std::mutex> lock(m);
        if (!made) {
            for (uint32_t i = 0; i < 256; ++i) {
                uint16_t c = static_cast<uint16_t>(i << 8);
                for (int b = 0; b < 8; ++b) {
                    c = static_cast<uint16_t>((c & 0x8000) ? (c << 1) ^ 0x8005 : c << 1);
                }
                table[i] = c;
            }
            made = true;
        }
    }
    uint16_t c = 0;
    for (std::size_t i = 0; i < n; ++i) {
        c = static_cast<uint16_t>((c << 8) ^ table[(c >> 8) ^ p[i]]);
    }
    return c;
}

void put_utf8_number(std::vector<uint8_t>& b, uint64_t v) {
    if (v < 0x80) {
        b.push_back(static_cast<uint8_t>(v));
        return;
    }
    int extra = 1;
    while (extra < 6 && v >= (uint64_t{1} << (5 * extra + 6))) ++extra;
    b.push_back(static_cast<uint8_t>((0xFF00u >> (extra + 1)) | (v >> (6 * extra))));
    for (int i = extra - 1; i >= 0; --i) {
        b.push_back(static_cast<uint8_t>(0x80 | ((v >> (6 * i)) & 0x3F)));
    }
}

// One channel's block as a FLAC subframe: constant, fixed predictor of order
// 0 to 4 with a partitioned Rice residual, or verbatim, whichever is shortest.
void encode_subframe(BitWriter& bw, const int32_t* x, std::size_t n, int bps) {
    bool constant = true;
    for (std::size_t i = 1; i < n && constant; ++i) constant = x[i] == x[0];
    if (constant) {
        bw.put(0, 8);   // pad 0, type CONSTANT (000000), no wasted bits
        bw.put(static_cast<uint32_t>(x[0]), bps);
        return;
    }

    int best_order = -1;
    uint64_t best_abs = ~uint64_t{0};
    std::vector<uint64_t> u;
    if (n > 4) {
        for (int order = 0; order <= 4; ++order) {
            uint64_t sum = 0;
            for (std::size_t i = static_cast<std::size_t>(order); i < n; ++i) {
                const int64_t a = x[i], b = i >= 1 ? x[i - 1] : 0, c = i >= 2 ? x[i - 2] : 0,
                              d = i >= 3 ? x[i - 3] : 0, e = i >= 4 ? x[i - 4] : 0;
                int64_t r = 0;
                switch (order) {
                    case 0: r = a; break;
                    case 1: r = a - b; break;
                    case 2: r = a - 2 * b + c; break;
                    case 3: r = a - 3 * b + 3 * c - d; break;
                    default: r = a - 4 * b + 6 * c - 4 * d + e; break;
                }
                sum += static_cast<uint64_t>(r < 0 ? -r : r);
            }
            if (sum < best_abs) {
                best_abs = sum;
                best_order = order;
            }
        }
    }

    int best_part = 0;
    uint64_t best_bits = static_cast<uint64_t>(n) * static_cast<uint64_t>(bps);   // verbatim
    std::vector<int> best_k;
    if (best_order >= 0) {
        const std::size_t order = static_cast<std::size_t>(best_order);
        const std::size_t count = n - order;
        u.resize(count);
        for (std::size_t i = order; i < n; ++i) {
            const int64_t a = x[i], b = i >= 1 ? x[i - 1] : 0, c = i >= 2 ? x[i - 2] : 0,
                          d = i >= 3 ? x[i - 3] : 0, e = i >= 4 ? x[i - 4] : 0;
            int64_t r = 0;
            switch (best_order) {
                case 0: r = a; break;
                case 1: r = a - b; break;
                case 2: r = a - 2 * b + c; break;
                case 3: r = a - 3 * b + 3 * c - d; break;
                default: r = a - 4 * b + 6 * c - 4 * d + e; break;
            }
            u[i - order] = static_cast<uint64_t>((r << 1) ^ (r >> 63));
        }
        const uint64_t head = 8 + order * static_cast<uint64_t>(bps) + 2 + 4;
        for (int p = 0; p <= 6; ++p) {
            const std::size_t parts = std::size_t{1} << p;
            if (n % parts != 0 || (n >> p) <= order) continue;
            const std::size_t per = n >> p;
            uint64_t bits = head;
            std::vector<int> ks(parts);
            for (std::size_t q = 0; q < parts; ++q) {
                const std::size_t lo = q == 0 ? 0 : q * per - order;
                const std::size_t hi = (q + 1) * per - order;
                uint64_t total = 0;
                for (std::size_t i = lo; i < hi; ++i) total += u[i];
                const double mean = static_cast<double>(total) / static_cast<double>(hi - lo);
                // Capped first: a mean past 2^25 would leave the search empty.
                const int k0 =
                    mean < 1.0 ? 0 : std::min(24, static_cast<int>(std::floor(std::log2(mean))));
                uint64_t best_cost = ~uint64_t{0};
                int best = 0;
                for (int k = std::max(0, k0 - 1); k <= std::min(24, k0 + 1); ++k) {
                    uint64_t cost = static_cast<uint64_t>(hi - lo) * static_cast<uint64_t>(k + 1);
                    for (std::size_t i = lo; i < hi; ++i) cost += u[i] >> k;
                    if (cost < best_cost) {
                        best_cost = cost;
                        best = k;
                    }
                }
                ks[q] = best;
                bits += 5 + best_cost;
            }
            if (bits < best_bits) {
                best_bits = bits;
                best_part = p;
                best_k = ks;
            }
        }
    }

    if (best_k.empty()) {   // verbatim
        bw.put(0, 1);
        bw.put(1, 6);
        bw.put(0, 1);
        for (std::size_t i = 0; i < n; ++i) bw.put(static_cast<uint32_t>(x[i]), bps);
        return;
    }
    const std::size_t order = static_cast<std::size_t>(best_order);
    bw.put(0, 1);
    bw.put(8u | static_cast<uint32_t>(best_order), 6);   // 001xxx FIXED
    bw.put(0, 1);
    for (std::size_t i = 0; i < order; ++i) bw.put(static_cast<uint32_t>(x[i]), bps);
    bw.put(1, 2);   // Rice, 5-bit parameters: a loud 24-bit residual wants k past 14
    bw.put(static_cast<uint32_t>(best_part), 4);
    const std::size_t parts = std::size_t{1} << best_part;
    const std::size_t per = n >> best_part;
    for (std::size_t q = 0; q < parts; ++q) {
        const std::size_t lo = q == 0 ? 0 : q * per - order;
        const std::size_t hi = (q + 1) * per - order;
        const int k = best_k[q];
        bw.put(static_cast<uint32_t>(k), 5);
        for (std::size_t i = lo; i < hi; ++i) {
            bw.unary_zeroes_then_one(u[i] >> k);
            bw.put(u[i], k);
        }
    }
}

}  // namespace

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
        const uint64_t total = std::ftell(file_) > 0 ? static_cast<uint64_t>(std::ftell(file_)) : 0;
        if (ok && c_ != ExportContainer::Aiff && total > 0xFFFFFFFFull) {
            error = path_ + " is over 4 GiB, which a WAV cannot hold";
            ok = false;
        }
        if (ok) {
            if (c_ == ExportContainer::Aiff) {
                patch(4, total - 8, 4, true);
                patch(frames_at_, frames_, 4, true);
                patch(data_size_at_, data_bytes_ + 8, 4, true);
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
    return true;
}

void OutFile::patch(std::size_t at, uint64_t v, int bytes, bool big) {
    std::vector<uint8_t> b;
    if (big) put_be(b, v, bytes);
    else put_le(b, v, bytes);
    const long keep = std::ftell(file_);
    std::fseek(file_, static_cast<long>(at), SEEK_SET);
    std::fwrite(b.data(), 1, b.size(), file_);
    std::fseek(file_, keep, SEEK_SET);
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
