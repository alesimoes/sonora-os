// SPDX-License-Identifier: GPL-3.0-or-later
//
// 32-bit float WAV (WAVE_FORMAT_IEEE_FLOAT), little-endian, for golden
// files only. Written byte by byte so the file is the same on every host.
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>

#include "sonora/measure/measure.h"

namespace sonora::measure {

namespace {

void put_u32(std::string& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void put_u16(std::string& b, std::uint16_t v) {
    b.push_back(static_cast<char>(v & 0xff));
    b.push_back(static_cast<char>(v >> 8));
}
std::uint32_t get_u32(const unsigned char* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t get_u16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

}  // namespace

bool write_wav_f32(const std::string& path, const Chans& x, double fs) {
    const auto ch = static_cast<std::uint16_t>(x.size());
    const std::uint32_t frames = x.empty() ? 0 : static_cast<std::uint32_t>(x[0].size());
    const std::uint32_t data = frames * ch * 4u;
    std::string b;
    b.reserve(44 + data);
    b += "RIFF";
    put_u32(b, 36 + data);
    b += "WAVEfmt ";
    put_u32(b, 16);
    put_u16(b, 3);  // IEEE float
    put_u16(b, ch);
    put_u32(b, static_cast<std::uint32_t>(fs));
    put_u32(b, static_cast<std::uint32_t>(fs) * ch * 4u);
    put_u16(b, static_cast<std::uint16_t>(ch * 4));
    put_u16(b, 32);
    b += "data";
    put_u32(b, data);
    for (std::uint32_t i = 0; i < frames; ++i) {
        for (std::uint16_t c = 0; c < ch; ++c) {
            std::uint32_t u;
            std::memcpy(&u, &x[c][i], 4);
            put_u32(b, u);
        }
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(b.data(), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(f);
}

bool read_wav_f32(const std::string& path, Chans& x, double& fs, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    const std::string b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const auto* p = reinterpret_cast<const unsigned char*>(b.data());
    if (b.size() < 12 || std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0) {
        error = path + " is not a RIFF/WAVE file";
        return false;
    }
    std::uint16_t ch = 0, fmt = 0, bits = 0;
    std::size_t pos = 12;
    while (pos + 8 <= b.size()) {
        const std::uint32_t size = get_u32(p + pos + 4);
        const unsigned char* body = p + pos + 8;
        if (std::memcmp(p + pos, "fmt ", 4) == 0 && size >= 16) {
            fmt = get_u16(body);
            ch = get_u16(body + 2);
            fs = get_u32(body + 4);
            bits = get_u16(body + 14);
        } else if (std::memcmp(p + pos, "data", 4) == 0) {
            if (fmt != 3 || bits != 32 || ch == 0) {
                error = path + " is not 32-bit float";
                return false;
            }
            const std::size_t frames = std::min<std::size_t>(size, b.size() - (pos + 8)) / (4u * ch);
            x.assign(ch, std::vector<float>(frames));
            for (std::size_t i = 0; i < frames; ++i) {
                for (std::uint16_t c = 0; c < ch; ++c) {
                    const std::uint32_t u = get_u32(body + 4 * (i * ch + c));
                    std::memcpy(&x[c][i], &u, 4);
                }
            }
            return true;
        }
        pos += 8 + size + (size & 1u);
    }
    error = path + " has no data chunk";
    return false;
}

}  // namespace sonora::measure
