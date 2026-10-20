// SPDX-License-Identifier: GPL-3.0-or-later
//
// How the engine's words name a timeline position: "frame N", N decimal
// frames from sample 0. One spelling for every refusal and notice, the real
// engine's and FakeEngine's alike, until positions travel structured with
// protocol 8. Allocates: never on the audio thread.
#pragma once

#include <cstdint>
#include <string>

namespace sonora {

inline std::string frame_text(uint64_t frame) { return "frame " + std::to_string(frame); }

}  // namespace sonora
