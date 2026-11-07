// SPDX-License-Identifier: GPL-3.0-or-later
//
// FilePeakSource: the Edit window's waveforms from the overview files. A
// source with a file answers from it; one without is built from its WAV on
// the worker; a stale file is rebuilt; an unreadable source stays silent and
// not ready, so Strip Silence waits rather than reading silence as quiet.
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "peak_source.h"
#include "sonora/peak_file.h"
#include "sonora/wav_writer.h"

#define OK(cond)                                                                   \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                              \
        }                                                                          \
    } while (0)

using namespace sonora;
using namespace sonora::ui;

namespace {

constexpr int64_t kFrames = 100000;

bool write_wav(const std::string& path, float level) {
    std::vector<float> in(static_cast<std::size_t>(kFrames), 0.0f);
    in[5000] = level;
    WavWriter w;
    return w.open(path, 48000, 1, WavWriter::Format::Float32, 0) &&
           w.write(in.data(), in.size()) && w.close();
}

}  // namespace

int main() {
    const std::string wav = "test_file_peaks.wav";
    const std::string peak = "test_file_peaks.wav.spk";
    std::remove(wav.c_str());
    std::remove(peak.c_str());
    OK(write_wav(wav, 0.5f));

    std::atomic<int> redraws{0};
    model::Frames length = kFrames;
    FilePeakSource src(
        [&](model::Id id, FilePeakSource::Located& out) {
            if (id == 9) return false;   // not a source of the session
            out.wav = id == 2 ? "test_file_peaks_missing.wav" : wav;
            out.peak = id == 2 ? "test_file_peaks_missing.wav.spk" : peak;
            out.length = length;
            return true;
        },
        [&] { ++redraws; });

    PeakPair p[2];
    // The first ask draws silence and starts the work; ready follows.
    src.peaks(1, 0, 4864, 256, 2, p);
    OK(p[0].max == 0.0f);
    src.wait_idle();
    OK(src.ready(1));
    OK(redraws.load() >= 1);
    OK(std::remove(peak.c_str()) == 0);   // the worker built the file beside the WAV
    // Bucket 4864..5119 holds the sample at 5000; the next holds none.
    src.peaks(1, 0, 4864, 256, 2, p);
    OK(p[0].max > 0.49f && p[0].max < 0.51f && p[0].min == 0.0f);
    OK(p[1].max == 0.0f);

    // A source that cannot be read: silent, never ready.
    src.peaks(2, 0, 0, 256, 1, p);
    src.wait_idle();
    OK(!src.ready(2) && p[0].max == 0.0f);
    // Not a source at all.
    OK(!src.ready(9));

    // The session says the source is another length than its file: the file
    // is not drawn as if it were the audio.
    length = kFrames / 2;
    src.peaks(1, 0, 4864, 256, 1, p);
    src.wait_idle();
    OK(!src.ready(1) && p[0].max == 0.0f);
    std::remove(wav.c_str());
    std::remove(peak.c_str());
    return 0;
}
