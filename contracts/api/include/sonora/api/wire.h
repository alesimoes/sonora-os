// SPDX-License-Identifier: GPL-3.0-or-later
//
// The wire codec: every message in messages.h to bytes and back.
//
// One message is one datagram (SOCK_SEQPACKET after M7): a type byte, then
// the fields in declaration order. Integers are little-endian whatever the
// host; a string is a u32 length and its bytes. Decoding never trusts its
// input: a short buffer, trailing bytes, a length past the cap, an enum
// value this version does not know, or the wrong type byte is an error with
// a reason, never a crash and never a partial message.
//
// Not on the audio thread: everything here allocates.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "sonora/api/messages.h"

namespace sonora::api::wire {

enum class Type : uint8_t {
    Hello = 1,
    CommandRequest = 2,
    ParameterChange = 3,
    ModelRequest = 4,
    CommandReply = 5,
    EngineEvent = 6,
    ModelUpdate = 7,
};

// A message's text fields; a session snapshot gets the larger cap.
constexpr std::size_t kMaxText = std::size_t{64} << 10;
constexpr std::size_t kMaxModelBytes = std::size_t{64} << 20;
constexpr std::size_t kMaxListEntries = 4096;

// The type byte, or false for an empty buffer.
bool peek_type(std::string_view bytes, Type& out);

void encode(const Hello& m, std::string& out);
void encode(const CommandRequest& m, std::string& out);
void encode(const ParameterChange& m, std::string& out);
void encode(const ModelRequest& m, std::string& out);
void encode(const CommandReply& m, std::string& out);
void encode(const EngineEvent& m, std::string& out);
void encode(const ModelUpdate& m, std::string& out);

bool decode(std::string_view bytes, Hello& out, std::string& error);
bool decode(std::string_view bytes, CommandRequest& out, std::string& error);
bool decode(std::string_view bytes, ParameterChange& out, std::string& error);
bool decode(std::string_view bytes, ModelRequest& out, std::string& error);
bool decode(std::string_view bytes, CommandReply& out, std::string& error);
bool decode(std::string_view bytes, EngineEvent& out, std::string& error);
bool decode(std::string_view bytes, ModelUpdate& out, std::string& error);

// Reads only the two version fields at the head of a Hello. They come first
// so that a client can name the mismatch even when everything after them
// changed shape in a newer engine.
bool decode_hello_versions(std::string_view bytes, uint32_t& protocol_version,
                           uint32_t& state_layout_version, std::string& error);

// The client's half of the version check, in words an operator can act on.
// Empty when the versions match.
std::string version_mismatch(uint32_t protocol_version, uint32_t state_layout_version);

}  // namespace sonora::api::wire
