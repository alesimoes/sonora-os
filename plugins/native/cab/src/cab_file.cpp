// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/cab_file.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <new>
#include <system_error>
#include <vector>

#include "sonora/wav_reader.h"

namespace sonora::native {

namespace {
// The header is the file's own claim; nothing is allocated from it before it
// is checked against these bounds and against the file's real size.
constexpr uint32_t kMinRate = 1000, kMaxRate = 768000;
constexpr uint32_t kMaxChannels = 8;
constexpr int64_t kChunkFrames = 4096;
}  // namespace

Cab::LoadResult load_cab_ir_wav(Cab& cab, const std::string& path, std::string& error) {
    try {
        sonora::WavReader r;
        if (!r.open(path, error)) return Cab::LoadResult::Invalid;
        const int64_t frames = r.frames();
        const uint32_t rate = r.sample_rate();
        const uint32_t ch = r.channels();
        if (frames <= 0 || ch == 0 || ch > kMaxChannels || rate < kMinRate || rate > kMaxRate ||
            frames > static_cast<int64_t>(rate) * 30) {
            error = "not an impulse response: empty, longer than 30 s, or an unusable header";
            return Cab::LoadResult::Invalid;
        }
        // Samples are at least 2 bytes, so a file smaller than that cannot hold
        // what the data chunk declares.
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || static_cast<uint64_t>(frames) * ch * 2 > size) {
            error = "the header declares more audio than the file holds";
            return Cab::LoadResult::Invalid;
        }
        std::vector<float> mono(static_cast<std::size_t>(frames));
        std::vector<float> chunk(static_cast<std::size_t>(kChunkFrames) * ch);
        for (int64_t at = 0; at < frames; at += kChunkFrames) {
            const int64_t n = std::min(kChunkFrames, frames - at);
            if (r.read(at, n, chunk.data()) != n) {
                error = "short read";
                return Cab::LoadResult::Invalid;
            }
            for (int64_t i = 0; i < n; ++i) mono[static_cast<std::size_t>(at + i)] = chunk[static_cast<std::size_t>(i) * ch];
        }
        return cab.load_ir(mono.data(), mono.size(), static_cast<double>(rate));
    } catch (const std::bad_alloc&) {
        error = "out of memory reading the response";
        return Cab::LoadResult::Invalid;
    }
}

std::unique_ptr<Cab> make_cab_plugin() {
    std::unique_ptr<Cab> cab = make_cab();
    cab->set_ir_reader(&load_cab_ir_wav);
    return cab;
}

}  // namespace sonora::native
