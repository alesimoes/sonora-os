// SPDX-License-Identifier: GPL-3.0-or-later
// The output file of an export: container headers, sample bytes and the
// patching done at close. Private to the library (not installed).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "sonora/audio_export.h"

namespace sonora {

constexpr std::size_t kBlockFrames = 4096;   // output frames per pass; also FLAC's block

class OutFile {
public:
    ~OutFile();

    bool open(const std::string& path, ExportContainer c, int bits, uint32_t rate, uint16_t ch,
              const ExportMeta& meta, std::string& error);

    // `il`: interleaved. Integers for 16/24, floats for 32.
    bool write_int(const int32_t* il, std::size_t frames, std::string& error);
    bool write_float(const float* il, std::size_t frames, std::string& error);

    bool close(std::string& error);
    void abort();

private:
    bool put(const std::vector<uint8_t>& b, std::string& error);
    void patch(std::size_t at, uint64_t v, int bytes, bool big);
    static void put_extended(std::vector<uint8_t>& b, uint32_t rate);
    static void append_bext(std::vector<uint8_t>& h, const ExportMeta& meta);

    bool flac_add(const int32_t* il, std::size_t frames, std::string& error);
    bool flac_frame(std::string& error);
    bool flac_finish(std::string& error);

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

}  // namespace sonora
