// SPDX-License-Identifier: GPL-3.0-or-later
//
// SHA-256 (FIPS 180-4), what names a content-addressed file beside the
// session: a plugin's state (ADR-0012 §6.1) and a MIDI sequence (ADR-0015
// §3). Here, in layer 0, because common/model may not depend on
// contracts/plugin, where it was first written. Allocates its result: never
// on the audio thread.
#pragma once

#include <string>
#include <string_view>

namespace sonora {

// SHA-256 of `bytes`, as 64 lower-case hex digits.
std::string sha256_hex(std::string_view bytes);

}  // namespace sonora
