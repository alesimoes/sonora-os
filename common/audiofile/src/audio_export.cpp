// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/audio_export.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

#include "sonora/wav_reader.h"

namespace sonora {

namespace {

constexpr std::size_t kBlockFrames = 4096;   // output frames per pass; also FLAC's block
constexpr int kSincZeroes = 32;              // resampler half width, in zero crossings
constexpr int kTableRes = 512;               // kernel table points per zero crossing
constexpr double kKaiserBeta = 10.0;
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

// --- the quantiser's noise ----------------------------------------------------

constexpr float kShapeC1 = 1.0f;    // NTF = 1 - z^-1 + 0.5 z^-2
constexpr float kShapeC2 = -0.5f;

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

// --- the output file -----------------------------------------------------------

class OutFile {
public:
    ~OutFile() { abort(); }

    bool open(const std::string& path, ExportContainer c, int bits, uint32_t rate, uint16_t ch,
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

    // `il`: interleaved. Integers for 16/24, floats for 32.
    bool write_int(const int32_t* il, std::size_t frames, std::string& error) {
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
    bool write_float(const float* il, std::size_t frames, std::string& error) {
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

    bool close(std::string& error) {
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

    void abort() {
        if (file_ != nullptr) {
            std::fclose(file_);
            file_ = nullptr;
        }
    }

private:
    bool put(const std::vector<uint8_t>& b, std::string& error) {
        if (!b.empty() && std::fwrite(b.data(), 1, b.size(), file_) != b.size()) {
            error = "cannot write " + path_ + ": " + std::strerror(errno);
            return false;
        }
        return true;
    }
    void patch(std::size_t at, uint64_t v, int bytes, bool big) {
        std::vector<uint8_t> b;
        if (big) put_be(b, v, bytes);
        else put_le(b, v, bytes);
        const long keep = std::ftell(file_);
        std::fseek(file_, static_cast<long>(at), SEEK_SET);
        std::fwrite(b.data(), 1, b.size(), file_);
        std::fseek(file_, keep, SEEK_SET);
    }

    // IEEE 754 extended, 80 bit, for AIFF's COMM sample rate.
    static void put_extended(std::vector<uint8_t>& b, uint32_t rate) {
        int e = 0;
        while ((uint64_t{rate} >> (e + 1)) != 0) ++e;
        put_be(b, static_cast<uint64_t>(16383 + e), 2);
        put_be(b, uint64_t{rate} << (63 - e), 8);
    }

    // EBU Tech 3285 v2: the origin of the file on the timeline, the loudness
    // the file was measured at, and when it was written.
    static void append_bext(std::vector<uint8_t>& h, const ExportMeta& meta) {
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

    bool flac_add(const int32_t* il, std::size_t frames, std::string& error) {
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

    bool flac_frame(std::string& error) {
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

    bool flac_finish(std::string& error) {
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

    std::FILE* file_ = nullptr;
    std::string path_;
    ExportContainer c_ = ExportContainer::Wav;
    int bits_ = 24;
    uint32_t rate_ = 48000;
    uint16_t ch_ = 2;
    uint64_t data_bytes_ = 0;
    uint64_t frames_ = 0;
    std::size_t data_size_at_ = 0;
    std::size_t frames_at_ = 0;
    // FLAC
    std::vector<int32_t> block_;
    std::size_t filled_ = 0;
    uint64_t frame_number_ = 0;
    uint64_t min_frame_ = ~uint64_t{0}, max_frame_ = 0, min_block_ = ~uint64_t{0}, max_block_ = 0;
    Md5 md5_;
    std::vector<uint8_t> md5_bytes_;
};

// --- the resampler -------------------------------------------------------------

double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 50; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

// Windowed sinc, one table of the low-pass at unit cutoff; a rate cut
// stretches it by the ratio.
class Kernel {
public:
    Kernel() {
        const std::size_t n = static_cast<std::size_t>(2 * kSincZeroes * kTableRes) + 2;
        table_.assign(n, 0.0f);
        const double pi = 3.14159265358979323846;
        const double i0b = bessel_i0(kKaiserBeta);
        for (std::size_t i = 0; i < n; ++i) {
            const double u = static_cast<double>(i) / kTableRes - kSincZeroes;
            if (std::fabs(u) >= kSincZeroes) continue;
            const double s = u == 0.0 ? 1.0 : std::sin(pi * u) / (pi * u);
            const double r = u / kSincZeroes;
            table_[i] = static_cast<float>(s * bessel_i0(kKaiserBeta * std::sqrt(1.0 - r * r)) / i0b);
        }
    }
    float at(double u) const {
        const double pos = (u + kSincZeroes) * kTableRes;
        if (pos < 0.0) return 0.0f;
        const std::size_t i = static_cast<std::size_t>(pos);
        if (i + 1 >= table_.size()) return 0.0f;
        const float f = static_cast<float>(pos - static_cast<double>(i));
        return table_[i] + (table_[i + 1] - table_[i]) * f;
    }

private:
    std::vector<float> table_;
};

}  // namespace

// --- Md5 -------------------------------------------------------------------------

namespace {
const uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
const int kMd5R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                       5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                       4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                       6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
}  // namespace

Md5::Md5() : s_{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476}, buf_{} {}

void Md5::block(const uint8_t* p) {
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = static_cast<uint32_t>(p[4 * i]) | (static_cast<uint32_t>(p[4 * i + 1]) << 8) |
               (static_cast<uint32_t>(p[4 * i + 2]) << 16) | (static_cast<uint32_t>(p[4 * i + 3]) << 24);
    }
    uint32_t a = s_[0], b = s_[1], c = s_[2], d = s_[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7 * i) % 16; }
        const uint32_t t = d;
        d = c;
        c = b;
        const uint32_t x = a + f + kMd5K[i] + m[g];
        b = b + ((x << kMd5R[i]) | (x >> (32 - kMd5R[i])));
        a = t;
    }
    s_[0] += a;
    s_[1] += b;
    s_[2] += c;
    s_[3] += d;
}

void Md5::update(const uint8_t* data, std::size_t n) {
    bytes_ += n;
    while (n > 0) {
        const std::size_t k = std::min(n, sizeof buf_ - fill_);
        std::memcpy(buf_ + fill_, data, k);
        fill_ += k;
        data += k;
        n -= k;
        if (fill_ == sizeof buf_) {
            block(buf_);
            fill_ = 0;
        }
    }
}

void Md5::finish(uint8_t out[16]) {
    const uint64_t bits = bytes_ * 8;
    const uint8_t one = 0x80;
    update(&one, 1);
    const uint8_t zero = 0;
    while (fill_ != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(bits >> (8 * i));
    update(len, 8);
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) out[4 * i + j] = static_cast<uint8_t>(s_[i] >> (8 * j));
    }
}

// --- Quantiser ----------------------------------------------------------------------

Quantiser::Quantiser(int bits, ExportDither dither, int channels, uint32_t seed)
    : bits_(bits), dither_(dither), rng_(seed != 0 ? seed : 1u) {
    (void)channels;
}

float Quantiser::next_uniform() {
    // xorshift32: the dither only has to be flat and not repeat within a file.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return static_cast<float>(rng_ >> 8) * (1.0f / 16777216.0f);
}

int32_t Quantiser::process(int channel, float x) {
    const int ch = std::min(std::max(channel, 0), 7);
    // A NaN would reach the integer cast; silence is the least bad sample.
    if (!std::isfinite(x)) x = 0.0f;
    const float scale = static_cast<float>(int64_t{1} << (bits_ - 1));
    float v = x * scale;
    if (dither_ == ExportDither::Shaped) v -= kShapeC1 * e1_[ch] + kShapeC2 * e2_[ch];
    float d = 0.0f;
    if (dither_ != ExportDither::None) d = next_uniform() - next_uniform();
    float q = std::nearbyint(v + d);
    q = std::min(std::max(q, -scale), scale - 1.0f);
    if (dither_ == ExportDither::Shaped) {
        // After a clip the error is not noise to be shaped; bound it.
        const float e = std::min(std::max(q - v, -2.0f), 2.0f);
        e2_[ch] = e1_[ch];
        e1_[ch] = e;
    }
    return static_cast<int32_t>(q);
}

// --- export ---------------------------------------------------------------------------

const char* export_extension(ExportContainer c) {
    switch (c) {
        case ExportContainer::Aiff: return ".aiff";
        case ExportContainer::Flac: return ".flac";
        default: return ".wav";
    }
}

std::string export_spec_problem(const ExportSpec& spec) {
    if (spec.bits != 16 && spec.bits != 24 && spec.bits != 32) {
        return "the bit depth is 16, 24 or 32-bit float";
    }
    if (spec.bits == 32 && (spec.container == ExportContainer::Flac ||
                            spec.container == ExportContainer::Aiff)) {
        return std::string(spec.container == ExportContainer::Flac ? "FLAC" : "AIFF") +
               " holds 16 or 24 bits, not 32-bit float";
    }
    if (spec.sample_rate != 0 && (spec.sample_rate < 8000 || spec.sample_rate > 384000)) {
        return "the sample rate is between 8000 and 384000 Hz";
    }
    return {};
}

bool export_audio(const std::string& src, const std::string& dst, const ExportSpec& spec,
                  const ExportMeta& meta, double gain_db, std::string& error) {
    error = export_spec_problem(spec);
    if (!error.empty()) return false;
    WavReader in;
    std::string why;
    if (!in.open(src, why)) {
        error = src + ": " + why;
        return false;
    }
    const uint16_t ch = in.channels();
    if (ch < 1 || ch > 2) {
        error = "export writes mono or stereo files";
        return false;
    }
    const uint32_t in_rate = in.sample_rate();
    const uint32_t out_rate = spec.sample_rate != 0 ? spec.sample_rate : in_rate;
    const bool convert = out_rate != in_rate;
    const int64_t in_frames = in.frames();
    const int64_t out_frames =
        convert ? static_cast<int64_t>(std::llround(static_cast<double>(in_frames) * out_rate / in_rate))
                : in_frames;
    ExportMeta m = meta;
    // The origin is a position in samples; it follows the rate.
    if (convert) {
        m.time_reference = static_cast<uint64_t>(
            std::llround(static_cast<double>(meta.time_reference) * out_rate / in_rate));
    }

    OutFile out;
    if (!out.open(dst, spec.container, spec.bits, out_rate, ch, m, error)) return false;
    const auto fail = [&](const std::string& because) {
        out.abort();
        std::remove(dst.c_str());
        error = because;
        return false;
    };

    const float gain = static_cast<float>(std::pow(10.0, gain_db / 20.0));
    const double step = static_cast<double>(in_rate) / static_cast<double>(out_rate);
    const double fc = std::min(1.0, static_cast<double>(out_rate) / in_rate);
    const double halfw = kSincZeroes / fc;
    Kernel kernel;
    Quantiser quant(spec.bits == 32 ? 24 : spec.bits, spec.dither, ch);
    std::vector<float> win, samples(kBlockFrames * ch);
    std::vector<int32_t> ints(kBlockFrames * ch);

    for (int64_t at = 0; at < out_frames; at += static_cast<int64_t>(kBlockFrames)) {
        const std::size_t k =
            static_cast<std::size_t>(std::min<int64_t>(kBlockFrames, out_frames - at));
        if (!convert) {
            if (in.read(at, static_cast<int64_t>(k), samples.data()) != static_cast<int64_t>(k)) {
                return fail(src + " cannot be read back to export it");
            }
        } else {
            const int64_t lo = static_cast<int64_t>(std::floor(static_cast<double>(at) * step - halfw)) + 1;
            const int64_t hi = static_cast<int64_t>(
                std::floor(static_cast<double>(at + static_cast<int64_t>(k) - 1) * step + halfw));
            win.assign(static_cast<std::size_t>(hi - lo + 1) * ch, 0.0f);
            const int64_t from = std::max<int64_t>(lo, 0);
            const int64_t to = std::min<int64_t>(hi + 1, in_frames);
            if (to > from) {
                if (in.read(from, to - from, win.data() + static_cast<std::size_t>(from - lo) * ch) !=
                    to - from) {
                    return fail(src + " cannot be read back to export it");
                }
            }
            for (std::size_t i = 0; i < k; ++i) {
                const double t = static_cast<double>(at + static_cast<int64_t>(i)) * step;
                const int64_t j0 = static_cast<int64_t>(std::floor(t - halfw)) + 1;
                const int64_t j1 = static_cast<int64_t>(std::floor(t + halfw));
                double a0 = 0.0, a1 = 0.0;
                for (int64_t j = j0; j <= j1; ++j) {
                    const double w = kernel.at((static_cast<double>(j) - t) * fc);
                    const std::size_t p = static_cast<std::size_t>(j - lo) * ch;
                    a0 += w * win[p];
                    if (ch == 2) a1 += w * win[p + 1];
                }
                samples[i * ch] = static_cast<float>(a0 * fc);
                if (ch == 2) samples[i * ch + 1] = static_cast<float>(a1 * fc);
            }
        }
        for (std::size_t i = 0; i < k * ch; ++i) samples[i] *= gain;
        if (spec.bits == 32) {
            if (!out.write_float(samples.data(), k, error)) return fail(error);
        } else {
            for (std::size_t i = 0; i < k; ++i) {
                for (uint16_t c = 0; c < ch; ++c) {
                    ints[i * ch + c] = quant.process(c, samples[i * ch + c]);
                }
            }
            if (!out.write_int(ints.data(), k, error)) return fail(error);
        }
    }
    if (!out.close(error)) return fail(error);
    return true;
}

}  // namespace sonora
