// SPDX-License-Identifier: GPL-3.0-or-later
//
// SHA-256 (FIPS 180-4), what names a content-addressed file beside the
// session: a plugin's state (ADR-0012 §6.1), a MIDI sequence (ADR-0015
// §3) and a file a plugin's state names (ADR-0029 §6). Here, in layer 0,
// because common/model may not depend on contracts/plugin, where it was
// first written. Allocates its result: never on the audio thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sonora {

// SHA-256 of `bytes`, as 64 lower-case hex digits.
std::string sha256_hex(std::string_view bytes);

// The same, fed in pieces: a plugin's sample set is hashed while it is
// copied, never held whole.
class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t size);
    // 64 lower-case hex digits. The hash is finished: update() no more.
    std::string hex();

private:
    void block(const unsigned char* p);

    uint32_t h_[8];
    unsigned char buf_[64];
    std::size_t held_ = 0;
    uint64_t total_ = 0;
};

}  // namespace sonora
