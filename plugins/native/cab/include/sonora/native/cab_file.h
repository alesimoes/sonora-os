// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reads a WAV impulse response with common/audiofile and hands it to a Cab.
// Control thread only: it opens a file and allocates. Its own library
// (sonora_native_cab_file), because common/audiofile is not in the neutral
// build; the Cab itself needs nothing but the standard library.
#pragma once

#include <string>

#include "sonora/native/cab.h"

namespace sonora::native {

// Reads channel 0 of a PCM16, PCM24 or float32 WAV of at most 30 s and calls
// cab.load_ir() with it. On a file problem returns Invalid and `error` says
// what is wrong with the file.
Cab::LoadResult load_cab_ir_wav(Cab& cab, const std::string& path, std::string& error);

// make_cab_plugin() (cab.h) is defined here.

}  // namespace sonora::native
