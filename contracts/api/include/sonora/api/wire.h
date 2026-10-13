// SPDX-License-Identifier: GPL-3.0-or-later
//
// The wire codec: every message in messages.h to bytes and back.
//
// A message is a type byte, then the fields in declaration order. Integers are little-endian whatever the
// host; a string is a u32 length and its bytes. Decoding never trusts its
// input: a short buffer, trailing bytes, a length past the cap, an enum
// value this version does not know, or the wrong type byte is an error with
// a reason, never a crash and never a partial message.
//
// A message crosses the transport as one or more frames (below), never as
// one datagram: M7's AF_UNIX SOCK_SEQPACKET socket refuses a datagram larger
// than its send buffer with EMSGSIZE, and a session's Snapshot is larger.
//
// Not on the audio thread: everything here allocates.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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
    PluginParameterChange = 8,   // protocol 3
    PluginCatalog = 9,           // protocol 3
};

// A message's text fields; a session snapshot gets the larger cap.
constexpr std::size_t kMaxText = std::size_t{64} << 10;
constexpr std::size_t kMaxModelBytes = std::size_t{64} << 20;
constexpr std::size_t kMaxListEntries = 4096;
// A catalog's plugins, and one plugin's parameters: the chain cap of
// ADR-0012 §5.1, which no one insert can pass either.
constexpr std::size_t kMaxCatalogPlugins = 4096;
constexpr std::size_t kMaxPluginParams = 8192;
// An enum parameter's labels, and a plugin's categories: far past any
// plugin's, short of a list that would make a catalog unreadable.
constexpr std::size_t kMaxParamLabels = 4096;
constexpr std::size_t kMaxPluginCategories = 16;
// A parameter key: an LV2 port symbol or a VST3 ParamID in decimal (ten
// digits). No real one comes near it.
constexpr std::size_t kMaxParamKeyBytes = 256;

// The type byte, or false for an empty buffer.
bool peek_type(std::string_view bytes, Type& out);

void encode(const Hello& m, std::string& out);
void encode(const CommandRequest& m, std::string& out);
void encode(const ParameterChange& m, std::string& out);
void encode(const ModelRequest& m, std::string& out);
void encode(const CommandReply& m, std::string& out);
void encode(const EngineEvent& m, std::string& out);
void encode(const ModelUpdate& m, std::string& out);
void encode(const PluginParameterChange& m, std::string& out);
// Refuses (false, `error`) a catalog past the caps above, which no receiver
// would take; nothing else in a catalog can fail to encode.
bool encode(const PluginCatalog& m, std::string& out, std::string& error);

bool decode(std::string_view bytes, Hello& out, std::string& error);
bool decode(std::string_view bytes, CommandRequest& out, std::string& error);
bool decode(std::string_view bytes, ParameterChange& out, std::string& error);
bool decode(std::string_view bytes, ModelRequest& out, std::string& error);
bool decode(std::string_view bytes, CommandReply& out, std::string& error);
bool decode(std::string_view bytes, EngineEvent& out, std::string& error);
bool decode(std::string_view bytes, ModelUpdate& out, std::string& error);
bool decode(std::string_view bytes, PluginParameterChange& out, std::string& error);
// Also refuses a catalog that would mislead an interface: a dense index out
// of place, a non-finite or inverted range, a flag this version does not
// define, one step, labels that are not one per step.
bool decode(std::string_view bytes, PluginCatalog& out, std::string& error);

// A client's half: whatever the engine sends to a client (a reply, an
// event, a model update, a catalog), decoded and appended to the inbox.
// False, with `error`, for what does not decode or is not a message an
// engine sends; the inbox is then untouched. Every EngineControl shares it,
// so a message kind added later reaches all of them at once.
bool decode_into(std::string_view bytes, Inbox& inbox, std::string& error);

// Reads only the two version fields at the head of a Hello. They come first
// so that a client can name the mismatch even when everything after them
// changed shape in a newer engine.
bool decode_hello_versions(std::string_view bytes, uint32_t& protocol_version,
                           uint32_t& state_layout_version, std::string& error);

// The client's half of the version check, in words an operator can act on.
// Empty when the versions match.
std::string version_mismatch(uint32_t protocol_version, uint32_t state_layout_version);

// --- frames ------------------------------------------------------------------
//
// One frame is one datagram: a u32 length of the whole message, a u32
// offset of this frame's bytes in it, then those bytes. A sender writes all
// of a message's frames, in order, before the next message's; SOCK_SEQPACKET
// keeps both the order and the boundaries, so a receiver reassembles one
// message at a time and never holds two.
//
// Frames rather than a memfd per large payload: the same code runs in
// process (InProcessEngine, FakeEngine) as across the socket, so it is
// exercised on every run, and no message brings a descriptor a receiver
// must remember to close.
//
// 64 KiB: under the default send buffer (net.core.wmem_default and
// wmem_max, 212992 bytes) with room for the kernel's per-datagram overhead,
// which counts against the buffer too.
constexpr std::size_t kMaxFrameBytes = std::size_t{64} << 10;
constexpr std::size_t kFrameHeaderBytes = 8;
// The most a receiver holds for one message: a Snapshot at kMaxModelBytes
// and its fixed fields, with room to spare. Checked against the first
// frame's header, before anything is held.
constexpr std::size_t kMaxMessageBytes = kMaxModelBytes + (std::size_t{1} << 20);

// Appends `message`'s frames to `frames`. Refuses an empty message and one
// over kMaxMessageBytes, which no receiver would take.
bool split_frames(std::string_view message, std::vector<std::string>& frames,
                  std::string& error);

class Reassembler {
public:
    enum class Result { Partial, Complete, Error };

    // Complete: `message` is the whole message. Error: the frame is out of
    // place (not where the message being reassembled goes on, a length over
    // the cap, a frame larger than one may be), `error` says how, and what
    // was held is dropped. The transport then closes the connection: a peer
    // that frames wrongly cannot be resynchronised.
    Result add(std::string_view frame, std::string& message, std::string& error);
    // No message half-received.
    bool idle() const { return expected_ == 0; }

private:
    Result fail(std::string& error, std::string why);

    std::string partial_;
    std::size_t expected_ = 0;
};

}  // namespace sonora::api::wire
