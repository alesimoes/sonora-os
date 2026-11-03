// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/native/cab_file.h"

#include <vector>

#include "sonora/wav_reader.h"

namespace sonora::native {

Cab::LoadResult load_cab_ir_wav(Cab& cab, const std::string& path, std::string& error) {
    sonora::WavReader r;
    if (!r.open(path, error)) return Cab::LoadResult::Invalid;
    const int64_t frames = r.frames();
    if (frames <= 0 || r.channels() == 0 || r.sample_rate() == 0 ||
        frames > static_cast<int64_t>(r.sample_rate()) * 30) {
        error = "not an impulse response: empty, or longer than 30 s";
        return Cab::LoadResult::Invalid;
    }
    const std::size_t ch = r.channels();
    std::vector<float> all(static_cast<std::size_t>(frames) * ch);
    if (r.read(0, frames, all.data()) != frames) {
        error = "short read";
        return Cab::LoadResult::Invalid;
    }
    std::vector<float> mono(static_cast<std::size_t>(frames));
    for (std::size_t i = 0; i < mono.size(); ++i) mono[i] = all[i * ch];
    return cab.load_ir(mono.data(), mono.size(), static_cast<double>(r.sample_rate()));
}

}  // namespace sonora::native
