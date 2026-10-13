// SPDX-License-Identifier: GPL-3.0-or-later
//
// The wire codec: every message survives a round trip field for field, and
// anything that is not a whole, known message is refused with a reason --
// never read past its end, never half-applied. Plus the command domains.
#include "sonora/api/wire.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "sonora/api/state_block.h"

using namespace sonora;
using namespace sonora::api;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

namespace {

bool mentions(const std::string& text, const char* needle) {
    if (text.find(needle) != std::string::npos) return true;
    std::fprintf(stderr, "  expected \"%s\" in: %s\n", needle, text.c_str());
    return false;
}

void round_trips() {
    std::string bytes, error;
    {
        Hello h;
        h.protocol_version = kProtocolVersion;
        h.state_layout_version = kStateLayoutVersion;
        h.sample_rate = 96000;
        h.buffer_frames = 64;
        h.track_count = 2;
        h.track_names = {"Kick", "Snare \xC3\xA9"};
        h.implemented_commands = {"transport.record", "track.toggle_arm"};
        wire::encode(h, bytes);
        Hello back;
        CHECK(wire::decode(bytes, back, error));
        CHECK(back.protocol_version == h.protocol_version);
        CHECK(back.state_layout_version == h.state_layout_version);
        CHECK(back.sample_rate == 96000 && back.buffer_frames == 64 && back.track_count == 2);
        CHECK(back.track_names == h.track_names);
        CHECK(back.implemented_commands == h.implemented_commands);
        uint32_t p = 0, l = 0;
        CHECK(wire::decode_hello_versions(bytes, p, l, error));
        CHECK(p == kProtocolVersion && l == kStateLayoutVersion);
    }
    {
        CommandRequest r;
        r.seq = 0x0102030405060708ull;
        r.command = "track.toggle_arm";
        r.target = -1;
        r.argument = std::string("a\0b", 3);
        wire::encode(r, bytes);
        CommandRequest back;
        CHECK(wire::decode(bytes, back, error));
        CHECK(back.seq == r.seq && back.command == r.command && back.target == -1 &&
              back.argument == r.argument);
    }
    {
        ParameterChange c;
        c.seq = 9;
        c.track = 17;
        c.param = ParamId::Gain;
        c.value = 0.125f;
        wire::encode(c, bytes);
        ParameterChange back;
        CHECK(wire::decode(bytes, back, error));
        CHECK(back.seq == 9 && back.track == 17 && back.param == ParamId::Gain &&
              back.value == 0.125f);
    }
    {
        ModelRequest m;
        m.seq = 3;
        m.action = ModelAction::Redo;
        m.op = "{\"op\":\"x\"}";
        wire::encode(m, bytes);
        ModelRequest back;
        CHECK(wire::decode(bytes, back, error));
        CHECK(back.seq == 3 && back.action == ModelAction::Redo && back.op == m.op);
    }
    {
        CommandReply r;
        r.seq = 4;
        r.result = DispatchResult::Failed;
        r.message = "arming cannot change during a take; stop first";
        wire::encode(r, bytes);
        CommandReply back;
        CHECK(wire::decode(bytes, back, error));
        CHECK(back.seq == 4 && back.result == DispatchResult::Failed && back.message == r.message);
    }
    {
        EngineEvent e;
        e.kind = EventKind::TakeFailed;
        e.track = -1;
        e.frame = 12;
        e.text = "take 12 failed";
        wire::encode(e, bytes);
        EngineEvent back;
        CHECK(wire::decode(bytes, back, error));
        CHECK(back.kind == EventKind::TakeFailed && back.track == -1 && back.frame == 12 &&
              back.text == e.text);
    }
    {
        ModelUpdate u;
        u.kind = ModelUpdateKind::Snapshot;
        u.revision = 40;
        u.undo_floor = 12;
        u.next_id = 99;
        u.bytes = std::string(100000, 'x');   // over kMaxText, under the model cap
        wire::encode(u, bytes);
        ModelUpdate back;
        CHECK(wire::decode(bytes, back, error));
        CHECK(back.kind == ModelUpdateKind::Snapshot && back.revision == 40 &&
              back.undo_floor == 12 && back.next_id == 99 && back.bytes == u.bytes);
    }
}

void refusals() {
    std::string bytes, error;
    CommandRequest r;
    r.command = "transport.record";
    wire::encode(r, bytes);

    // Every truncation is refused, and none reads past the end (ASan).
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        CommandRequest back;
        back.command = "untouched";
        CHECK(!wire::decode(std::string_view(bytes.data(), n), back, error));
        CHECK(back.command == "untouched");
    }
    CHECK(mentions(error, "cut short"));

    CommandRequest back;
    CHECK(!wire::decode(bytes + "x", back, error));
    CHECK(mentions(error, "left over"));

    // The wrong message type is not reinterpreted.
    CommandReply reply;
    CHECK(!wire::decode(bytes, reply, error));
    CHECK(mentions(error, "type"));

    // A length past the cap is refused before anything is allocated for it.
    std::string huge = bytes.substr(0, 9);
    huge += std::string("\xff\xff\xff\x7f", 4);
    CHECK(!wire::decode(huge, back, error));
    CHECK(mentions(error, "cap"));

    // An enum value from a newer engine is named, not cast.
    EngineEvent e;
    wire::encode(e, bytes);
    bytes[1] = 99;
    EngineEvent eb;
    CHECK(!wire::decode(bytes, eb, error));
    CHECK(mentions(error, "unknown"));

    // A Hello whose name list disagrees with its count.
    Hello h;
    h.protocol_version = kProtocolVersion;
    h.state_layout_version = kStateLayoutVersion;
    h.track_count = 3;
    h.track_names = {"one"};
    wire::encode(h, bytes);
    Hello hb;
    CHECK(!wire::decode(bytes, hb, error));
    CHECK(mentions(error, "tracks"));
}

void version_check() {
    CHECK(wire::version_mismatch(kProtocolVersion, kStateLayoutVersion).empty());
    const std::string p = wire::version_mismatch(kProtocolVersion + 1, kStateLayoutVersion);
    CHECK(mentions(p, "refusing to connect"));
    CHECK(mentions(p, ("protocol version " + std::to_string(kProtocolVersion + 1)).c_str()));
    CHECK(mentions(p, ("speaks version " + std::to_string(kProtocolVersion)).c_str()));
    const std::string l = wire::version_mismatch(kProtocolVersion, kStateLayoutVersion + 7);
    CHECK(mentions(l, "layout version"));
    CHECK(mentions(l, std::to_string(kStateLayoutVersion + 7).c_str()));

    // The versions are read first, so a Hello from a newer engine whose
    // later fields changed shape still names the mismatch.
    std::string bytes;
    Hello h;
    h.protocol_version = kProtocolVersion + 1;
    h.state_layout_version = kStateLayoutVersion;
    wire::encode(h, bytes);
    bytes += "a newer engine's extra fields";
    uint32_t pv = 0, lv = 0;
    std::string error;
    CHECK(wire::decode_hello_versions(bytes, pv, lv, error));
    CHECK(pv == kProtocolVersion + 1);
}

void domains() {
    // The ids are the contract with every keymap: none may change here.
    CHECK(find_command("transport.toggle_record") != nullptr);
    CHECK(find_command("app.quit") != nullptr);
    for (const auto& spec : command_catalog()) {
        const std::string id = spec.id;
        const bool interface = id.rfind("tool.", 0) == 0 || id.rfind("zoom.", 0) == 0 ||
                               id.rfind("edit.mode.", 0) == 0 || id == "app.quit";
        if (interface != (spec.domain == CommandDomain::Interface)) {
            std::fprintf(stderr, "FAIL %s is in the wrong domain\n", spec.id);
            ++failures;
        }
        CHECK(is_engine_command(id) == !interface);
    }
    CHECK(!is_engine_command("no.such.command"));
}

// The bytes a frame header holds, for building bad frames by hand.
std::string header(uint32_t total, uint32_t offset) {
    std::string h;
    for (int i = 0; i < 4; ++i) h.push_back(static_cast<char>((total >> (8 * i)) & 0xFF));
    for (int i = 0; i < 4; ++i) h.push_back(static_cast<char>((offset >> (8 * i)) & 0xFF));
    return h;
}

// A Snapshot of a large session is far over what one AF_UNIX datagram can
// carry (EMSGSIZE above the socket's send buffer, about 208 KiB by
// default), so every message crosses as frames of at most kMaxFrameBytes
// and is reassembled whole on the other side.
void frames_carry_large_messages() {
    std::string error;
    ModelUpdate u;
    u.kind = ModelUpdateKind::Snapshot;
    u.revision = 7;
    u.undo_floor = 3;
    u.next_id = 1000;
    u.bytes.resize(3 * 1024 * 1024 + 17);
    for (std::size_t i = 0; i < u.bytes.size(); ++i) {
        u.bytes[i] = static_cast<char>((i * 131) & 0xFF);
    }
    std::string message;
    wire::encode(u, message);
    std::vector<std::string> frames;
    CHECK(wire::split_frames(message, frames, error));
    CHECK(frames.size() > 1);
    std::size_t carried = 0;
    for (const std::string& f : frames) {
        CHECK(f.size() <= wire::kMaxFrameBytes);
        carried += f.size() - wire::kFrameHeaderBytes;
    }
    CHECK(carried == message.size());

    wire::Reassembler r;
    std::string whole;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto got = r.add(frames[i], whole, error);
        CHECK(got == (i + 1 < frames.size() ? wire::Reassembler::Result::Partial
                                            : wire::Reassembler::Result::Complete));
        CHECK(i + 1 == frames.size() || !r.idle());
    }
    CHECK(r.idle());
    CHECK(whole == message);
    ModelUpdate back;
    CHECK(wire::decode(whole, back, error));
    CHECK(back.bytes == u.bytes && back.revision == 7 && back.undo_floor == 3);

    // A small message is one frame.
    CommandRequest c;
    c.command = "transport.record";
    wire::encode(c, message);
    frames.clear();
    CHECK(wire::split_frames(message, frames, error));
    CHECK(frames.size() == 1);
    CHECK(r.add(frames[0], whole, error) == wire::Reassembler::Result::Complete);
    CHECK(whole == message);

    // Exactly one frame's worth, and one byte more.
    const std::size_t room = wire::kMaxFrameBytes - wire::kFrameHeaderBytes;
    frames.clear();
    CHECK(wire::split_frames(std::string(room, 'a'), frames, error) && frames.size() == 1);
    frames.clear();
    CHECK(wire::split_frames(std::string(room + 1, 'a'), frames, error) && frames.size() == 2);

    // Nothing to send, or more than any receiver holds, is refused.
    frames.clear();
    CHECK(!wire::split_frames("", frames, error) && frames.empty());
    CHECK(!wire::split_frames(std::string(wire::kMaxMessageBytes + 1, 'a'), frames, error));
    CHECK(frames.empty());
    CHECK(mentions(error, "cap"));
}

// Reassembly trusts nothing: a frame out of place, a length past the cap,
// a message that changes size, a frame too big for a datagram. Each is an
// error with a reason, leaves nothing half-held, and the next message
// reassembles cleanly. After M7 the socket is closed on an error.
void frames_refuse_what_is_out_of_place() {
    std::string error, whole;
    const auto refused = [&](std::vector<std::string> frames, const char* why) {
        wire::Reassembler r;
        wire::Reassembler::Result got = wire::Reassembler::Result::Partial;
        for (const std::string& f : frames) {
            got = r.add(f, whole, error);
            if (got != wire::Reassembler::Result::Partial) break;
        }
        bool ok = got == wire::Reassembler::Result::Error && r.idle() && mentions(error, why);
        // Recovers: a good message right after.
        std::vector<std::string> good;
        std::string e2;
        ok = wire::split_frames("x", good, e2) && ok;
        ok = r.add(good[0], whole, e2) == wire::Reassembler::Result::Complete && whole == "x" && ok;
        return ok;
    };
    CHECK(refused({"abc"}, "shorter than"));
    CHECK(refused({header(0, 0)}, "empty"));
    CHECK(refused({header(10, 0)}, "carries nothing"));
    CHECK(refused({header(static_cast<uint32_t>(wire::kMaxMessageBytes + 1), 0) + "a"}, "cap"));
    CHECK(refused({header(10, 4) + "abcd"}, "offset 4"));
    CHECK(refused({header(10, 0) + "abcd", header(10, 6) + "ef"}, "offset 6"));
    CHECK(refused({header(10, 0) + "abcd", header(12, 4) + "ef"}, "12 bytes"));
    CHECK(refused({header(4, 0) + "abcdef"}, "past"));
    CHECK(refused({header(10, 0) + "abcd", header(10, 4) + "efghijk"}, "past"));
    CHECK(refused({header(10, 0) + std::string(wire::kMaxFrameBytes, 'a')}, "frame of"));
    // A second message starting before the first ended.
    CHECK(refused({header(10, 0) + "abcd", header(3, 0) + "xyz"}, "offset 0"));
}

}  // namespace

int main() {
    round_trips();
    refusals();
    version_check();
    domains();
    frames_carry_large_messages();
    frames_refuse_what_is_out_of_place();
    if (failures) return EXIT_FAILURE;
    std::printf("api wire: ok\n");
    return EXIT_SUCCESS;
}
