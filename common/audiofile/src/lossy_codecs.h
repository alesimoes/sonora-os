// SPDX-License-Identifier: GPL-3.0-or-later
// The encoders this build carries (ADR-0182). Private to the library. Each
// factory exists only when CMake found its libraries.
#pragma once

#include <memory>

#include "sonora/lossy_encoder.h"

namespace sonora {

#ifdef SONORA_HAVE_OPUS
std::unique_ptr<LossyEncoder> make_opus_encoder(LossyByteSink& sink);
#endif
#ifdef SONORA_HAVE_VORBIS
std::unique_ptr<LossyEncoder> make_vorbis_encoder(LossyByteSink& sink);
#endif

}  // namespace sonora
