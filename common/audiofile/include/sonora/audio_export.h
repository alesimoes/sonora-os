// SPDX-License-Identifier: GPL-3.0-or-later
//
// Delivery files (interchange.audio.export): a float32 file in, one of BWF,
// WAV, AIFF or FLAC out at 16, 24 or 32-bit float, at another rate if asked,
// with dither when the depth is cut.
//
// Everything here runs on a worker or the control thread, never in
// Session::process(): it reads and writes files, allocates and may take
// seconds. The FLAC encoder is ours (fixed predictors and Rice coding):
// libFLAC would be one more package in every build image and a vendored copy
// on MSVC, for a file the reference decoder already verifies sample by sample.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace sonora {

enum class ExportContainer { Bwf, Wav, Aiff, Flac };
enum class ExportDither { None, Tpdf, Shaped };

struct ExportSpec {
    ExportContainer container = ExportContainer::Bwf;
    // 16 or 24 (integer), 32 (IEEE float: WAV and BWF only).
    int bits = 24;
    uint32_t sample_rate = 0;   // 0: the source's
    ExportDither dither = ExportDither::None;
};

struct ExportMeta {
    std::string description;        // bext Description
    uint64_t time_reference = 0;    // bext TimeReference, samples at the source rate
    double loudness_lufs = -200.0;  // bext LoudnessValue when above -70
};

// The file extension of a container, with its dot.
const char* export_extension(ExportContainer c);

// Why a spec cannot be written ("FLAC holds 16 or 24 bits, not 32-bit
// float"), empty when it can.
std::string export_spec_problem(const ExportSpec& spec);

// Reads `src` (any file WavReader reads, stereo), multiplies by `gain_db`,
// converts the rate, dithers, and writes `dst` (created, or truncated if
// present). On failure `error` says why and `dst` is removed.
bool export_audio(const std::string& src, const std::string& dst, const ExportSpec& spec,
                  const ExportMeta& meta, double gain_db, std::string& error);

// --- pieces, exposed for the tests ---

// Float in full scale (+-1) to a signed integer of `bits`, with the state of
// up to 8 channels. TPDF is +-1 LSB, so the error does not follow the signal.
// Shaped adds a second-order error feedback that takes the noise out of the
// band below about 10 kHz (at 48 kHz) and puts it in the top octave.
class Quantiser {
public:
    Quantiser(int bits, ExportDither dither, int channels, uint32_t seed = 0x2545F491u);
    int32_t process(int channel, float x);

private:
    float next_uniform();
    int bits_;
    ExportDither dither_;
    uint32_t rng_;
    float e1_[8] = {};
    float e2_[8] = {};
};

// Streaming MD5 (FLAC's STREAMINFO checksum).
class Md5 {
public:
    Md5();
    void update(const uint8_t* data, std::size_t n);
    void finish(uint8_t out[16]);

private:
    void block(const uint8_t* p);
    uint32_t s_[4];
    uint64_t bytes_ = 0;
    uint8_t buf_[64];
    std::size_t fill_ = 0;
};

}  // namespace sonora
