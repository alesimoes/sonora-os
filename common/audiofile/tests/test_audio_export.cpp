// SPDX-License-Identifier: GPL-3.0-or-later
//
// interchange.audio.export: every container at every depth is written and
// read back by a parser that shares no code with the writer (the parsers
// below are the formats' specifications, read cold), the bext fields hold
// what was given, dither has the statistics it is for, and the rate
// conversion keeps a tone and drops one above the new Nyquist.
#include "sonora/audio_export.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "sonora/wav_writer.h"

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            return 1;                                                              \
        }                                                                          \
    } while (0)

namespace {

using namespace sonora;

constexpr double kPi = 3.14159265358979323846;

bool slurp(const std::string& path, std::vector<uint8_t>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    uint8_t b[65536];
    std::size_t n;
    out.clear();
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) out.insert(out.end(), b, b + n);
    std::fclose(f);
    return true;
}

uint64_t le(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
uint64_t be(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}
int64_t sext(uint64_t v, int bits) {
    const uint64_t m = uint64_t{1} << (bits - 1);
    return static_cast<int64_t>((v ^ m) - m);
}

struct Decoded {
    uint32_t rate = 0;
    uint16_t channels = 0;
    int bits = 0;
    bool is_float = false;
    uint64_t frames = 0;
    std::vector<double> s;   // interleaved, full scale +-1
    // bext
    bool has_bext = false;
    std::string description, originator, date, time;
    uint64_t time_reference = 0;
    int version = -1, loudness = 0;
    size_t bext_size = 0;
};

bool parse_wav(const std::vector<uint8_t>& b, Decoded& d) {
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(&b[8], "WAVE", 4) != 0) {
        return false;
    }
    if (le(&b[4], 4) != b.size() - 8) return false;   // the RIFF size is the file's
    size_t at = 12;
    const uint8_t* data = nullptr;
    uint64_t data_bytes = 0;
    while (at + 8 <= b.size()) {
        const uint64_t n = le(&b[at + 4], 4);
        const uint8_t* c = &b[at + 8];
        if (at + 8 + n > b.size()) return false;
        if (std::memcmp(&b[at], "fmt ", 4) == 0) {
            d.is_float = le(c, 2) == 3;
            d.channels = static_cast<uint16_t>(le(c + 2, 2));
            d.rate = static_cast<uint32_t>(le(c + 4, 4));
            d.bits = static_cast<int>(le(c + 14, 2));
            if (le(c + 8, 4) != uint64_t{d.rate} * d.channels * (d.bits / 8)) return false;
            if (le(c + 12, 2) != uint64_t{d.channels} * (d.bits / 8)) return false;
        } else if (std::memcmp(&b[at], "bext", 4) == 0) {
            d.has_bext = true;
            d.bext_size = n;
            d.description = std::string(reinterpret_cast<const char*>(c));
            d.originator = std::string(reinterpret_cast<const char*>(c + 256));
            d.date = std::string(reinterpret_cast<const char*>(c + 320), 10);
            d.time = std::string(reinterpret_cast<const char*>(c + 330), 8);
            d.time_reference = le(c + 338, 8);
            d.version = static_cast<int>(le(c + 346, 2));
            d.loudness = static_cast<int16_t>(le(c + 412, 2));
        } else if (std::memcmp(&b[at], "data", 4) == 0) {
            data = c;
            data_bytes = n;
        }
        at += 8 + n + (n & 1);
    }
    if (data == nullptr || d.channels == 0) return false;
    const int bytes = d.bits / 8;
    d.frames = data_bytes / (d.channels * bytes);
    d.s.resize(d.frames * d.channels);
    for (size_t i = 0; i < d.s.size(); ++i) {
        const uint8_t* p = data + i * bytes;
        if (d.is_float) {
            uint32_t u = static_cast<uint32_t>(le(p, 4));
            float f;
            std::memcpy(&f, &u, 4);
            d.s[i] = f;
        } else {
            d.s[i] = static_cast<double>(sext(le(p, bytes), d.bits)) / std::ldexp(1.0, d.bits - 1);
        }
    }
    return true;
}

bool parse_aiff(const std::vector<uint8_t>& b, Decoded& d) {
    if (b.size() < 12 || std::memcmp(b.data(), "FORM", 4) != 0 || std::memcmp(&b[8], "AIFF", 4) != 0) {
        return false;
    }
    if (be(&b[4], 4) != b.size() - 8) return false;
    size_t at = 12;
    uint64_t frames = 0;
    while (at + 8 <= b.size()) {
        const uint64_t n = be(&b[at + 4], 4);
        const uint8_t* c = &b[at + 8];
        if (at + 8 + n > b.size()) return false;
        if (std::memcmp(&b[at], "COMM", 4) == 0) {
            d.channels = static_cast<uint16_t>(be(c, 2));
            frames = be(c + 2, 4);
            d.bits = static_cast<int>(be(c + 6, 2));
            const int exp = static_cast<int>(be(c + 8, 2)) - 16383;
            const uint64_t mant = be(c + 10, 8);
            d.rate = static_cast<uint32_t>(std::llround(std::ldexp(static_cast<double>(mant), exp - 63)));
        } else if (std::memcmp(&b[at], "SSND", 4) == 0) {
            if (be(c, 4) != 0) return false;
            const int bytes = d.bits / 8;
            if (n - 8 != frames * d.channels * bytes) return false;
            d.frames = frames;
            d.s.resize(frames * d.channels);
            for (size_t i = 0; i < d.s.size(); ++i) {
                d.s[i] = static_cast<double>(sext(be(c + 8 + i * bytes, bytes), d.bits)) /
                         std::ldexp(1.0, d.bits - 1);
            }
        }
        at += 8 + n + (n & 1);
    }
    return !d.s.empty() || frames == 0;
}

// FLAC, from the format specification: CRCs and the MD5 checked.
uint8_t ref_crc8(const uint8_t* p, size_t n) {
    uint8_t c = 0;
    for (size_t i = 0; i < n; ++i) {
        for (int k = 7; k >= 0; --k) {
            const bool top = (c & 0x80) != 0;
            c = static_cast<uint8_t>(c << 1);
            if (top != (((p[i] >> k) & 1) != 0)) c ^= 0x07;
        }
    }
    return c;
}
uint16_t ref_crc16(const uint8_t* p, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; ++i) {
        for (int k = 7; k >= 0; --k) {
            const bool top = (c & 0x8000) != 0;
            c = static_cast<uint16_t>(c << 1);
            if (top != (((p[i] >> k) & 1) != 0)) c ^= 0x8005;
        }
    }
    return c;
}

struct Bits {
    const uint8_t* p;
    size_t n;
    size_t pos = 0;   // in bits
    bool bad = false;
    uint64_t get(int k) {
        uint64_t v = 0;
        for (int i = 0; i < k; ++i) {
            if (pos >= n * 8) {
                bad = true;
                return 0;
            }
            v = (v << 1) | ((p[pos >> 3] >> (7 - (pos & 7))) & 1);
            ++pos;
        }
        return v;
    }
    int64_t sget(int k) { return sext(get(k), k); }
};

bool parse_flac(const std::vector<uint8_t>& b, Decoded& d, bool& md5_ok) {
    if (b.size() < 42 || std::memcmp(b.data(), "fLaC", 4) != 0) return false;
    if (b[4] != 0x80 || be(&b[5], 3) != 34) return false;
    Bits si{&b[8], 34};
    si.get(16 + 16 + 24 + 24);
    d.rate = static_cast<uint32_t>(si.get(20));
    d.channels = static_cast<uint16_t>(si.get(3) + 1);
    d.bits = static_cast<int>(si.get(5) + 1);
    d.frames = si.get(36);
    const uint8_t* md5 = &b[8 + 18];
    size_t at = 42;
    std::vector<std::vector<int64_t>> ch(d.channels);
    uint64_t number = 0;
    while (at < b.size()) {
        const uint8_t* f = &b[at];
        if (f[0] != 0xFF || f[1] != 0xF8) return false;
        const int bs_code = f[2] >> 4;
        if ((f[2] & 0x0F) != 0) return false;   // the rate comes from STREAMINFO
        if ((f[3] >> 4) != d.channels - 1) return false;
        const int size_code = (f[3] >> 1) & 7;
        if (size_code != (d.bits == 16 ? 4 : 6)) return false;
        size_t h = 4;
        // The frame number, UTF-8 coded.
        int extra = 0;
        uint64_t num = f[h];
        if (num >= 0xC0) {
            extra = num >= 0xFC ? 5 : num >= 0xF8 ? 4 : num >= 0xF0 ? 3 : num >= 0xE0 ? 2 : 1;
            num &= (0x3Fu >> extra);
            for (int i = 1; i <= extra; ++i) num = (num << 6) | (f[h + i] & 0x3F);
        }
        h += 1 + extra;
        if (num != number++) return false;
        size_t n = 0;
        if (bs_code == 12) n = 4096;
        else if (bs_code == 7) { n = be(f + h, 2) + 1; h += 2; }
        else return false;
        if (ref_crc8(f, h) != f[h]) return false;
        Bits bits{f + h + 1, b.size() - at - h - 1};
        std::vector<std::vector<int64_t>> sub(d.channels);
        for (uint16_t c = 0; c < d.channels; ++c) {
            if (bits.get(1) != 0) return false;
            const int type = static_cast<int>(bits.get(6));
            if (bits.get(1) != 0) return false;
            std::vector<int64_t>& x = sub[c];
            x.assign(n, 0);
            if (type == 0) {
                const int64_t v = bits.sget(d.bits);
                for (auto& s : x) s = v;
            } else if (type == 1) {
                for (auto& s : x) s = bits.sget(d.bits);
            } else if (type >= 8 && type <= 12) {
                const int order = type - 8;
                for (int i = 0; i < order; ++i) x[i] = bits.sget(d.bits);
                const int method = static_cast<int>(bits.get(2));
                if (method > 1) return false;
                const int po = static_cast<int>(bits.get(4));
                const size_t per = n >> po;
                for (size_t q = 0; q < (size_t{1} << po); ++q) {
                    const int k = static_cast<int>(bits.get(method == 0 ? 4 : 5));
                    if (k == (method == 0 ? 15 : 31)) return false;   // escape: unused here
                    const size_t cnt = q == 0 ? per - order : per;
                    size_t i = q == 0 ? order : q * per;
                    for (size_t j = 0; j < cnt; ++j, ++i) {
                        uint64_t quo = 0;
                        while (bits.get(1) == 0 && !bits.bad) ++quo;
                        const uint64_t u = (quo << k) | bits.get(k);
                        const int64_t r = static_cast<int64_t>(u >> 1) ^ -static_cast<int64_t>(u & 1);
                        int64_t p = 0;
                        switch (order) {
                            case 1: p = x[i - 1]; break;
                            case 2: p = 2 * x[i - 1] - x[i - 2]; break;
                            case 3: p = 3 * x[i - 1] - 3 * x[i - 2] + x[i - 3]; break;
                            case 4: p = 4 * x[i - 1] - 6 * x[i - 2] + 4 * x[i - 3] - x[i - 4]; break;
                            default: break;
                        }
                        x[i] = p + r;
                    }
                }
            } else {
                return false;
            }
            if (bits.bad) return false;
        }
        if (bits.pos % 8 != 0) bits.pos += 8 - bits.pos % 8;
        const size_t end = h + 1 + bits.pos / 8;
        if (at + end + 2 > b.size()) return false;
        if (ref_crc16(f, end) != be(f + end, 2)) return false;
        for (uint16_t c = 0; c < d.channels; ++c) ch[c].insert(ch[c].end(), sub[c].begin(), sub[c].end());
        at += end + 2;
    }
    const size_t frames = ch[0].size();
    if (frames != d.frames) return false;
    d.s.resize(frames * d.channels);
    // The MD5 of the samples as little-endian integers, interleaved.
    std::vector<uint8_t> raw;
    for (size_t i = 0; i < frames; ++i) {
        for (uint16_t c = 0; c < d.channels; ++c) {
            const int64_t v = ch[c][i];
            d.s[i * d.channels + c] = static_cast<double>(v) / std::ldexp(1.0, d.bits - 1);
            for (int k = 0; k < d.bits / 8; ++k) raw.push_back(static_cast<uint8_t>(v >> (8 * k)));
        }
    }
    Md5 m;
    m.update(raw.data(), raw.size());
    uint8_t digest[16];
    m.finish(digest);
    md5_ok = std::memcmp(digest, md5, 16) == 0;
    return true;
}

// 1 s stereo at 48 kHz: two tones, noise, a clipped stretch, then silence on
// the left so FLAC meets a constant subframe and a short last block.
float src_l(int i) {
    if (i >= 43000) return 0.0f;
    if (i >= 20000 && i < 20100) return 2.6f;   // over full scale after -6 dB: clips
    return static_cast<float>(0.5 * std::sin(2 * kPi * 440.0 * i / 48000.0) +
                              0.1 * std::sin(2 * kPi * 3000.0 * i / 48000.0));
}
float src_r(int i) {
    const uint32_t h = static_cast<uint32_t>(i) * 2654435761u;
    const float noise = (static_cast<float>(h >> 8) / 16777216.0f - 0.5f) * 0.02f;
    return static_cast<float>(0.3 * std::sin(2 * kPi * 997.0 * i / 48000.0)) + noise;
}

bool make_source(const std::string& path, int frames, int rate) {
    std::remove(path.c_str());
    WavWriter w;
    if (!w.open(path, static_cast<uint32_t>(rate), 2, WavWriter::Format::Float32, 0)) return false;
    std::vector<float> il(static_cast<size_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        il[2 * i] = src_l(i);
        il[2 * i + 1] = src_r(i);
    }
    return w.write(il.data(), static_cast<size_t>(frames)) && w.close();
}

int round_trip_each_format() {
    const std::string src = "test_export_src.wav";
    constexpr int kFrames = 48000;
    CHECK(make_source(src, kFrames, 48000));
    struct Case { ExportContainer c; int bits; ExportDither dither; };
    const Case cases[] = {
        {ExportContainer::Bwf, 16, ExportDither::Tpdf},  {ExportContainer::Bwf, 24, ExportDither::None},
        {ExportContainer::Bwf, 32, ExportDither::None},  {ExportContainer::Wav, 16, ExportDither::None},
        {ExportContainer::Wav, 24, ExportDither::Tpdf},  {ExportContainer::Wav, 32, ExportDither::None},
        {ExportContainer::Aiff, 16, ExportDither::Shaped}, {ExportContainer::Aiff, 24, ExportDither::None},
        {ExportContainer::Flac, 16, ExportDither::Tpdf}, {ExportContainer::Flac, 24, ExportDither::None},
        {ExportContainer::Flac, 16, ExportDither::None},
    };
    int n = 0;
    for (const Case& k : cases) {
        ExportSpec spec;
        spec.container = k.c;
        spec.bits = k.bits;
        spec.dither = k.dither;
        ExportMeta meta;
        meta.description = "Bounce of Test";
        meta.time_reference = 123456;
        meta.loudness_lufs = -16.25;
        // SONORA_EXPORT_KEEP=dir leaves the files there for tools/test-export-interop.sh.
        const char* keep = std::getenv("SONORA_EXPORT_KEEP");
        const std::string dst = (keep != nullptr ? std::string(keep) + "/" : std::string()) +
                                "export_" + std::to_string(n) + "_" + std::to_string(k.bits) +
                                export_extension(k.c);
        ++n;
        std::remove(dst.c_str());
        std::string err;
        const bool ok = export_audio(src, dst, spec, meta, -6.0, err);
        if (!ok) std::fprintf(stderr, "export %s: %s\n", dst.c_str(), err.c_str());
        CHECK(ok);
        std::vector<uint8_t> bytes;
        CHECK(slurp(dst, bytes));
        Decoded d;
        bool md5_ok = true;
        switch (k.c) {
            case ExportContainer::Aiff: CHECK(parse_aiff(bytes, d)); break;
            case ExportContainer::Flac: CHECK(parse_flac(bytes, d, md5_ok)); break;
            default: CHECK(parse_wav(bytes, d)); break;
        }
        CHECK(md5_ok);
        CHECK(d.rate == 48000 && d.channels == 2 && d.bits == k.bits);
        CHECK(d.frames == static_cast<uint64_t>(kFrames));
        CHECK(d.is_float == (k.bits == 32));
        // The tolerance: one LSB without dither, three with (TPDF is +-1,
        // and the shaped error adds its two earlier ones).
        const double lsb = std::ldexp(1.0, -(k.bits == 32 ? 24 : k.bits - 1));
        // The float path (gain, then rounding) is good to a few 1e-8 itself.
        const double slack = k.bits == 32 ? 0.0 : 1.5e-7;
        const double tol = slack + (k.bits == 32 ? 1e-6
                           : k.dither == ExportDither::None ? 0.51 * lsb
                           : k.dither == ExportDither::Tpdf ? 1.6 * lsb : 4.1 * lsb);
        const double gain = std::pow(10.0, -6.0 / 20.0);
        for (int i = 0; i < kFrames; ++i) {
            double l = std::min(std::max(src_l(i) * gain, -1.0), 1.0 - lsb);
            double r = src_r(i) * gain;
            if (k.bits == 32) l = src_l(i) * gain;
            if (std::fabs(d.s[2 * i] - l) > tol || std::fabs(d.s[2 * i + 1] - r) > tol) {
                std::fprintf(stderr, "%s frame %d: %g vs %g / %g vs %g tol %g\n", dst.c_str(), i,
                             d.s[2 * i], l, d.s[2 * i + 1], r, tol);
                return 1;
            }
        }
        // bext: BWF only, version 2, the fields as given.
        CHECK(d.has_bext == (k.c == ExportContainer::Bwf));
        if (k.c == ExportContainer::Bwf) {
            CHECK(d.bext_size == 602);
            CHECK(d.description == "Bounce of Test" && d.originator == "Sonora Engine");
            CHECK(d.time_reference == 123456 && d.version == 2);
            CHECK(d.loudness == -1625);
            CHECK(d.date.size() == 10 && d.date[4] == '-' && d.date[7] == '-' && d.date[0] == '2');
            CHECK(d.time.size() == 8 && d.time[2] == ':' && d.time[5] == ':');
        }
        if (keep == nullptr) std::remove(dst.c_str());
    }
    std::remove(src.c_str());
    return 0;
}

int flac_is_smaller_than_pcm_and_handles_blocks() {
    // Lengths on and around the block size, and none at all.
    for (int frames : {0, 1, 3, 4095, 4096, 4097, 8192, 9000}) {
        const std::string src = "test_export_flac_src.wav";
        CHECK(make_source(src, frames, 44100));
        ExportSpec spec;
        spec.container = ExportContainer::Flac;
        spec.bits = 24;
        std::string err;
        const std::string dst = "test_export_flac_len.flac";
        const bool ok = export_audio(src, dst, spec, ExportMeta{}, 0.0, err);
        if (!ok) std::fprintf(stderr, "flac %d: %s\n", frames, err.c_str());
        CHECK(ok);
        std::vector<uint8_t> bytes;
        CHECK(slurp(dst, bytes));
        Decoded d;
        bool md5_ok = false;
        CHECK(parse_flac(bytes, d, md5_ok) && md5_ok);
        CHECK(d.frames == static_cast<uint64_t>(frames) && d.rate == 44100);
        if (frames == 9000) {
            std::fprintf(stderr, "flac, 9000 frames: %zu bytes of %zu PCM\n", bytes.size(),
                         static_cast<size_t>(frames) * 6);
            // Noise on one side holds it near 18 of 24 bits.
            CHECK(bytes.size() < static_cast<size_t>(frames) * 6 * 9 / 10);
        }
        std::remove(dst.c_str());
        std::remove(src.c_str());
    }
    return 0;
}

int refuses_what_a_container_cannot_hold() {
    ExportSpec s;
    s.container = ExportContainer::Flac;
    s.bits = 32;
    CHECK(export_spec_problem(s).find("FLAC") != std::string::npos);
    s.container = ExportContainer::Aiff;
    CHECK(export_spec_problem(s).find("AIFF") != std::string::npos);
    s.container = ExportContainer::Wav;
    CHECK(export_spec_problem(s).empty());
    s.bits = 20;
    CHECK(!export_spec_problem(s).empty());
    s.bits = 16;
    s.sample_rate = 100;
    CHECK(!export_spec_problem(s).empty());
    // And a failed export leaves no file.
    CHECK(make_source("test_export_bad_src.wav", 100, 48000));
    s.sample_rate = 0;
    s.container = ExportContainer::Flac;
    s.bits = 32;
    std::string err;
    std::remove("test_export_bad.flac");
    CHECK(!export_audio("test_export_bad_src.wav", "test_export_bad.flac", s, ExportMeta{}, 0.0, err));
    std::FILE* f = std::fopen("test_export_bad.flac", "rb");
    CHECK(f == nullptr);
    std::remove("test_export_bad_src.wav");
    return 0;
}

int dither_has_its_statistics() {
    // A constant a third of an LSB above zero: without dither it always
    // rounds to 0 (the error is the signal), with TPDF its mean comes back
    // and the error is white at 1/4 LSB^2 (1/12 rounding + 1/6 dither).
    constexpr int kN = 200000;
    const float x = 0.3f / 32768.0f;
    {
        Quantiser q(16, ExportDither::None, 1);
        double sum = 0;
        for (int i = 0; i < kN; ++i) sum += q.process(0, x);
        CHECK(sum == 0.0);
    }
    for (ExportDither mode : {ExportDither::Tpdf, ExportDither::Shaped}) {
        Quantiser q(16, mode, 1);
        std::vector<double> e(kN);
        double mean = 0;
        for (int i = 0; i < kN; ++i) {
            e[i] = q.process(0, x) - 0.3;
            mean += e[i];
        }
        mean /= kN;
        double var = 0, r1 = 0;
        for (int i = 0; i < kN; ++i) var += (e[i] - mean) * (e[i] - mean);
        for (int i = 1; i < kN; ++i) r1 += (e[i] - mean) * (e[i - 1] - mean);
        var /= kN;
        r1 /= kN;
        if (mode == ExportDither::Tpdf) {
            // Unbiased, level as theory, and no memory.
            CHECK(std::fabs(mean) < 0.01);
            CHECK(std::fabs(var - 0.25) < 0.01);
            CHECK(std::fabs(r1 / var) < 0.01);
        } else {
            // Second order shaping, NTF 1 - z^-1 + 0.5 z^-2: the lag-1
            // correlation is (-1 - 0.5) / (1 + 1 + 0.25) = -2/3, and the
            // total power rises by 2.25 (the noise went up the spectrum).
            CHECK(std::fabs(mean) < 0.02);
            CHECK(std::fabs(r1 / var + 2.0 / 3.0) < 0.02);
            CHECK(std::fabs(var - 0.25 * 2.25) < 0.04);
        }
    }
    // Silence stays silence without dither, and full scale clips, not wraps.
    Quantiser z(16, ExportDither::None, 2);
    CHECK(z.process(0, 0.0f) == 0 && z.process(1, 0.0f) == 0);
    CHECK(z.process(0, 1.0f) == 32767 && z.process(0, -1.0f) == -32768);
    CHECK(z.process(0, 7.0f) == 32767);
    CHECK(z.process(0, std::nanf("")) == 0);
    Quantiser t(16, ExportDither::Tpdf, 1);
    for (int i = 0; i < 1000; ++i) {
        const int32_t v = t.process(0, 1.0f);
        CHECK(v == 32767);   // clips, never wraps
        (void)v;
    }
    return 0;
}

double tone_amplitude(const std::vector<float>& il, uint32_t rate, double hz, size_t from, size_t to) {
    double re = 0, im = 0;
    for (size_t i = from; i < to; ++i) {
        const double w = 2 * kPi * hz * static_cast<double>(i) / rate;
        re += il[2 * i] * std::cos(w);
        im += il[2 * i] * std::sin(w);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(to - from);
}

int converts_the_rate() {
    struct Pair { int from, to; };
    for (Pair p : {Pair{48000, 44100}, Pair{44100, 96000}, Pair{96000, 44100}, Pair{48000, 48000}}) {
        const std::string src = "test_export_sr_src.wav";
        const int frames = p.from;   // 1 s
        std::remove(src.c_str());
        WavWriter w;
        CHECK(w.open(src, static_cast<uint32_t>(p.from), 2, WavWriter::Format::Float32, 0));
        std::vector<float> il(static_cast<size_t>(frames) * 2);
        for (int i = 0; i < frames; ++i) {
            // 1 kHz at 0.5 in the left; the right holds a tone above the new
            // Nyquist when the rate falls (30 kHz at 96k), else 2 kHz.
            il[2 * i] = static_cast<float>(0.5 * std::sin(2 * kPi * 1000.0 * i / p.from));
            const double hz = p.from == 96000 ? 30000.0 : 2000.0;
            il[2 * i + 1] = static_cast<float>(0.5 * std::sin(2 * kPi * hz * i / p.from));
        }
        CHECK(w.write(il.data(), static_cast<size_t>(frames)) && w.close());
        ExportSpec spec;
        spec.container = ExportContainer::Wav;
        spec.bits = 32;
        spec.sample_rate = static_cast<uint32_t>(p.to);
        ExportMeta meta;
        meta.time_reference = static_cast<uint64_t>(p.from);   // one second in
        const std::string dst = "test_export_sr_dst.wav";
        std::string err;
        const bool ok = export_audio(src, dst, spec, meta, 0.0, err);
        if (!ok) std::fprintf(stderr, "sr: %s\n", err.c_str());
        CHECK(ok);
        std::vector<uint8_t> bytes;
        CHECK(slurp(dst, bytes));
        Decoded d;
        CHECK(parse_wav(bytes, d));
        CHECK(d.rate == static_cast<uint32_t>(p.to));
        CHECK(d.frames == static_cast<uint64_t>(p.to));   // 1 s stays 1 s
        std::vector<float> out(d.s.size());
        for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<float>(d.s[i]);
        // Away from the file's ends, the tone is kept to 0.1 % ...
        const double a = tone_amplitude(out, d.rate, 1000.0, 2000, d.frames - 2000);
        CHECK(std::fabs(a - 0.5) < 0.0005);
        std::vector<float> right(d.s.size());
        for (size_t i = 0; i < d.frames; ++i) right[2 * i] = out[2 * i + 1];
        if (p.from == 96000) {
            // ... and 30 kHz, above the new Nyquist, is 60 dB down at least
            // (it folds to 14.1 kHz otherwise).
            CHECK(tone_amplitude(right, d.rate, 14100.0, 2000, d.frames - 2000) < 0.0005);
        } else {
            CHECK(std::fabs(tone_amplitude(right, d.rate, 2000.0, 2000, d.frames - 2000) - 0.5) < 0.0005);
        }
        // The bext origin follows the rate (here, not a BWF: Wav has none).
        CHECK(!d.has_bext);
        std::remove(src.c_str());
        std::remove(dst.c_str());
    }
    return 0;
}

int bwf_origin_follows_the_rate() {
    const std::string src = "test_export_origin_src.wav";
    CHECK(make_source(src, 48000, 48000));
    ExportSpec spec;
    spec.container = ExportContainer::Bwf;
    spec.sample_rate = 96000;
    ExportMeta meta;
    meta.time_reference = 24000;
    std::string err;
    CHECK(export_audio(src, "test_export_origin.wav", spec, meta, 0.0, err));
    std::vector<uint8_t> bytes;
    CHECK(slurp("test_export_origin.wav", bytes));
    Decoded d;
    CHECK(parse_wav(bytes, d));
    CHECK(d.time_reference == 48000 && d.rate == 96000);
    CHECK(d.loudness == 0x7FFF);   // no loudness given: "not given"
    std::remove(src.c_str());
    std::remove("test_export_origin.wav");
    return 0;
}

int md5_matches_the_standard_vectors() {
    const auto hex = [](const char* s) {
        Md5 m;
        m.update(reinterpret_cast<const uint8_t*>(s), std::strlen(s));
        uint8_t d[16];
        m.finish(d);
        char out[33];
        for (int i = 0; i < 16; ++i) std::snprintf(out + 2 * i, 3, "%02x", d[i]);
        return std::string(out);
    };
    CHECK(hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(hex("12345678901234567890123456789012345678901234567890123456789012345678901234567890") ==
          "57edf4a22be3c955ac49da2e2107b67a");
    return 0;
}

}  // namespace

int main() {
    if (md5_matches_the_standard_vectors()) return 1;
    if (refuses_what_a_container_cannot_hold()) return 1;
    if (dither_has_its_statistics()) return 1;
    if (round_trip_each_format()) return 1;
    if (flac_is_smaller_than_pcm_and_handles_blocks()) return 1;
    if (converts_the_rate()) return 1;
    if (bwf_origin_follows_the_rate()) return 1;
    std::printf("audio_export: ok\n");
    return 0;
}
