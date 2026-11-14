// SPDX-License-Identifier: GPL-3.0-or-later
//
// Opus and Vorbis delivery files (ADR-0182): the render is read, resampled
// (Opus: 48 kHz) and gained like any export, then encoded through the lossy
// seam. Worker thread only.
#pragma once

#include <cstdint>
#include <string>

#include "sonora/audio_export.h"
#include "sonora/lossy_encoder.h"

namespace sonora {

// Registers the codecs whose libraries CMake found. Absent libraries register
// nothing: that codec is then refused at the start of a bounce.
bool register_builtin_lossy_codecs(LossyRegistry& registry, std::string& error);

// `bitrate` b/s for the whole stream. On failure `error` says why and `dst`
// is not left behind; a codec not in `registry` fails before `dst` is made.
bool export_lossy(const std::string& src, const std::string& dst, LossyCodec codec,
                  uint32_t bitrate, const ExportMeta& meta, double gain_db,
                  const LossyRegistry& registry, std::string& error);

}  // namespace sonora
