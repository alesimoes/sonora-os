// SPDX-License-Identifier: GPL-3.0-or-later
//
// The wire codec: every message survives a round trip field for field, and
// anything that is not a whole, known message is refused with a reason --
// never read past its end, never half-applied. Plus the command domains.
#include "sonora/api/wire.h"

#include <cstdio>
#include <cstdlib>
#include <limits>
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
        m.action = ModelAction::FlushPluginState;
        m.op = "42";
        wire::encode(m, bytes);
        CHECK(wire::decode(bytes, back, error));
        CHECK(back.action == ModelAction::FlushPluginState && back.op == "42");
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
              back.insert == 0 && back.text == e.text);
        e.kind = EventKind::PluginFault;
        e.frame = 0;
        e.insert = 0x0102030405060708ull;
        wire::encode(e, bytes);
        CHECK(wire::decode(bytes, back, error));
        CHECK(back.kind == EventKind::PluginFault && back.insert == e.insert && back.frame == 0);
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
        CHECK(back.label.empty());
    }
    {
        // An Undo names the step it undid (protocol 4), for "Undo Paste".
        ModelUpdate u;
        u.kind = ModelUpdateKind::Undo;
        u.revision = 41;
        u.label = "Paste \xC3\xA9";
        wire::encode(u, bytes);
        ModelUpdate back;
        CHECK(wire::decode(bytes, back, error));
        CHECK(back.kind == ModelUpdateKind::Undo && back.label == u.label);
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

    // MIDI sequence files (ADR-0015, M-1c) are version 8: a Load carries a
    // session bundle, and the undo window a missing sequence's reference. A
    // version-7 interface, which would send a bare session file that names
    // files the engine cannot read, reads this engine's Hello and refuses it
    // at connect, naming both numbers. Its check is this function with its
    // own constants, passed here.
    CHECK(kProtocolVersion == 8);
    Hello engine;
    engine.protocol_version = kProtocolVersion;
    engine.state_layout_version = kStateLayoutVersion;
    wire::encode(engine, bytes);
    CHECK(wire::decode_hello_versions(bytes, pv, lv, error));
    const std::string v7 = wire::version_mismatch(pv, lv, 7, kStateLayoutVersion);
    CHECK(mentions(v7, "the engine speaks control protocol version 8 and this interface "
                       "speaks version 7"));
    CHECK(mentions(v7, "same build"));
    // And this interface refuses a version-7 engine the same way.
    CHECK(mentions(wire::version_mismatch(7, kStateLayoutVersion),
                   "the engine speaks control protocol version 7 and this interface "
                   "speaks version 8"));
}

void domains() {
    // The ids are the contract with every keymap: none may change here.
    CHECK(find_command("transport.toggle_record") != nullptr);
    CHECK(find_command("app.quit") != nullptr);
    for (const auto& spec : command_catalog()) {
        const std::string id = spec.id;
        const bool interface = id.rfind("tool.", 0) == 0 || id.rfind("zoom.", 0) == 0 ||
                               id.rfind("edit.mode.", 0) == 0 || id.rfind("focus.", 0) == 0 ||
                               id == "app.quit" || id == "window.transport" ||
                               id == "window.undo_history";
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

PluginCatalog sample_catalog() {
    PluginCatalog c;
    c.generation = 0x1122334455667788ull;
    c.plugins.resize(2);
    PluginInfo& a = c.plugins[0];
    a.format = PluginFormat::Lv2;
    a.id = "http://gareus.org/oss/lv2/dpl#mono";
    a.name = "x42-dpl \xC3\xA9";
    a.vendor = "x42";
    a.version = "0.9.2";
    a.categories = {"Dynamics", "Limiter"};
    a.inputs = a.outputs = 1;
    a.has_editor = true;
    PluginParam q;
    q.index = 0;
    q.key = "threshold";
    q.name = "Threshold";
    q.min = -10.0f;
    q.max = 0.0f;
    q.default_value = -1.0f;
    q.flags = kParamAutomatable | kParamLogarithmic;
    q.unit = "dB";
    a.params.push_back(q);
    q.index = 1;
    q.key = "enable";
    q.flags = kParamToggle | kParamPastCap;
    q.min = 0.0f;
    q.max = 1.0f;
    q.default_value = 1.0f;
    q.unit.clear();
    q.steps = 2;
    a.params.push_back(q);
    q.index = 2;
    q.key = "mode";
    q.flags = kParamEnum;
    q.max = 2.0f;
    q.default_value = 0.0f;
    q.steps = 3;
    q.labels = {"Peak", "RMS", "True peak"};
    a.params.push_back(q);
    PluginInfo& b = c.plugins[1];
    b.format = PluginFormat::Vst3;
    b.id = "565354436F6D703A6C73702D636F6D70";
    b.inputs = 2;
    b.outputs = 2;
    b.usable = false;
    b.refusal = "the scan timed out";
    return c;
}

// Protocol 3 (ADR-0012 §8): the plugin messages, and the one inbox decoder
// every EngineControl shares.
void plugin_messages() {
    std::string bytes, error;
    {
        PluginParameterChange c;
        c.seq = 41;
        c.insert = 0x00ABCDEF01234567ull;
        c.key = "4294967295";
        c.value = -0.75f;
        wire::encode(c, bytes);
        PluginParameterChange back;
        CHECK(wire::decode(bytes, back, error));
        CHECK(back.seq == 41 && back.insert == c.insert && back.key == "4294967295" &&
              back.value == -0.75f);
        CHECK(!wire::decode(bytes + "x", back, error) && mentions(error, "left over"));
        // A key no plugin has: longer than any LV2 symbol or ParamID needs.
        c.key.assign(wire::kMaxParamKeyBytes + 1, 'k');
        wire::encode(c, bytes);
        CHECK(!wire::decode(bytes, back, error) && mentions(error, "over the cap"));
    }
    const PluginCatalog c = sample_catalog();
    CHECK(wire::encode(c, bytes, error));
    PluginCatalog back;
    CHECK(wire::decode(bytes, back, error));
    CHECK(back.generation == c.generation && back.plugins.size() == 2);
    const PluginInfo& a = back.plugins[0];
    CHECK(a.format == PluginFormat::Lv2 && a.id == c.plugins[0].id && a.name == c.plugins[0].name);
    CHECK(a.vendor == "x42" && a.version == "0.9.2" && a.inputs == 1 && a.outputs == 1);
    CHECK(a.usable && a.refusal.empty() && a.has_editor && a.params.size() == 3);
    CHECK(a.categories == c.plugins[0].categories);
    CHECK(a.params[0].key == "threshold" && a.params[0].min == -10.0f &&
          a.params[0].max == 0.0f && a.params[0].default_value == -1.0f &&
          a.params[0].flags == (kParamAutomatable | kParamLogarithmic));
    CHECK(a.params[0].unit == "dB" && a.params[0].steps == 0 && a.params[0].labels.empty());
    CHECK(a.params[1].index == 1 && a.params[1].flags == (kParamToggle | kParamPastCap));
    CHECK(a.params[1].steps == 2 && a.params[1].labels.empty());
    CHECK(a.params[2].steps == 3 && a.params[2].labels == c.plugins[0].params[2].labels);
    const PluginInfo& b = back.plugins[1];
    CHECK(b.format == PluginFormat::Vst3 && !b.usable && b.refusal == "the scan timed out");
    CHECK(!b.has_editor && b.params.empty() && b.inputs == 2 && b.categories.empty());

    // Each refusal names what is wrong, and leaves the output alone.
    const auto refused = [&](PluginCatalog bad, const char* says) {
        std::string enc, err;
        if (!wire::encode(bad, enc, err)) return mentions(err, says);
        PluginCatalog out;
        out.generation = 7;
        return !wire::decode(enc, out, err) && mentions(err, says) && out.generation == 7;
    };
    PluginCatalog bad = c;
    bad.plugins[0].params[1].index = 5;
    CHECK(refused(bad, "dense index 5"));
    bad = c;
    bad.plugins[0].params[0].min = std::numeric_limits<float>::quiet_NaN();
    CHECK(refused(bad, "range"));
    bad = c;
    bad.plugins[0].params[0].max = -20.0f;
    CHECK(refused(bad, "range"));
    bad = c;
    bad.plugins[0].params[0].default_value = 3.0f;
    CHECK(refused(bad, "default"));
    bad = c;
    bad.plugins[0].params[0].flags = 1u << 9;
    CHECK(refused(bad, "flags"));
    bad = c;
    bad.plugins[0].format = static_cast<PluginFormat>(2);
    CHECK(refused(bad, "enum value 2"));
    bad = c;
    bad.plugins.resize(wire::kMaxCatalogPlugins + 1);
    CHECK(refused(bad, "over the cap"));
    // Steps and labels that cannot be drawn as a choice.
    bad = c;
    bad.plugins[0].params[2].steps = 1;
    bad.plugins[0].params[2].labels.resize(1);
    CHECK(refused(bad, "1 step"));
    bad = c;
    bad.plugins[0].params[2].labels.pop_back();
    CHECK(refused(bad, "2 labels for 3 steps"));
    bad = c;
    bad.plugins[0].params[0].labels = {"a"};
    CHECK(refused(bad, "1 labels for 0 steps"));
    bad = c;
    bad.plugins[0].params[2].steps = static_cast<uint32_t>(wire::kMaxParamLabels + 1);
    bad.plugins[0].params[2].labels.resize(wire::kMaxParamLabels + 1);
    CHECK(refused(bad, "over the cap"));
    bad = c;
    bad.plugins[0].categories.resize(wire::kMaxPluginCategories + 1);
    CHECK(refused(bad, "over the cap"));
    // A flag byte that is neither 0 nor 1. `usable` follows the outputs:
    // type, generation, count, format, four strings, categories (a count),
    // inputs, outputs.
    std::string enc;
    PluginCatalog one;
    one.plugins.resize(1);
    CHECK(wire::encode(one, enc, error));
    const std::size_t usable_at = 1 + 8 + 4 + 4 + 4 * 4 + 4 + 4 + 4;
    enc[usable_at] = 2;
    CHECK(!wire::decode(enc, back, error) && mentions(error, "a flag of 2"));
    // A count past the cap is refused before anything is reserved for it.
    enc.clear();
    CHECK(wire::encode(one, enc, error));
    enc[9] = '\xFF';
    enc[10] = '\xFF';
    CHECK(!wire::decode(enc, back, error) && mentions(error, "over the cap"));

    // decode_into: each message an engine sends lands in its list.
    Inbox inbox;
    CHECK(wire::encode(c, bytes, error) && wire::decode_into(bytes, inbox, error));
    CommandReply r;
    r.seq = 3;
    wire::encode(r, bytes);
    CHECK(wire::decode_into(bytes, inbox, error));
    EngineEvent e;
    e.kind = EventKind::PluginFault;
    e.insert = 9;
    e.text = "x42-dpl (slot J) crashed";
    wire::encode(e, bytes);
    CHECK(wire::decode_into(bytes, inbox, error));
    ModelUpdate m;
    m.revision = 4;
    wire::encode(m, bytes);
    CHECK(wire::decode_into(bytes, inbox, error));
    CHECK(inbox.catalogs.size() == 1 && inbox.catalogs[0].plugins.size() == 2);
    CHECK(inbox.replies.size() == 1 && inbox.replies[0].seq == 3);
    CHECK(inbox.events.size() == 1 && inbox.events[0].kind == EventKind::PluginFault &&
          inbox.events[0].insert == 9);
    CHECK(inbox.model.size() == 1 && inbox.model[0].revision == 4);
    // What a client sends is not for a client's inbox.
    PluginParameterChange pc;
    wire::encode(pc, bytes);
    CHECK(!wire::decode_into(bytes, inbox, error) && mentions(error, "not one an engine sends"));
    CHECK(!wire::decode_into("", inbox, error));
    inbox.clear();
    CHECK(inbox.catalogs.empty());
}

}  // namespace

int main() {
    round_trips();
    plugin_messages();
    refusals();
    version_check();
    domains();
    frames_carry_large_messages();
    frames_refuse_what_is_out_of_place();
    if (failures) return EXIT_FAILURE;
    std::printf("api wire: ok\n");
    return EXIT_SUCCESS;
}
