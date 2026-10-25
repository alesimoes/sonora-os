// SPDX-License-Identifier: GPL-3.0-or-later
// HostControl, wire version 1 (ADR-0012 §4): every message round-trips, the
// golden encodings of the hellos are pinned (their first bytes never move),
// every truncation and every trailing byte is refused, forged counts and
// lengths cannot make a decoder hold memory, a fuzzed byte stream never
// crashes a decoder or the reassembler, frames reassemble through a Link
// over a transport that blocks at random, and the state blob, its SHA-256
// file name and the --version line are what the engine checks.
#include "sonora/plugin/host_control.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace sonora::plugin;
using namespace sonora::plugin::control;

static_assert(kHostWireVersion == 2, "the golden encodings below describe wire version 2");

namespace {

int failures = 0;
#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

std::string hex(const std::string& s) {
    static const char d[] = "0123456789abcdef";
    std::string o;
    for (unsigned char c : s) {
        o.push_back(d[c >> 4]);
        o.push_back(d[c & 15]);
    }
    return o;
}

const std::string kVstId = "84E8DE5F92554F5396FAE4133C935A18";

// Round trip, then: every strict prefix refused, one trailing byte refused,
// a wrong type byte refused. Returns the decoded copy.
template <class M>
M round_trip(const M& m, const char* name) {
    std::string bytes, error;
    encode(m, bytes);
    M back;
    if (!decode(bytes, back, error)) {
        std::printf("FAIL %s does not decode: %s\n", name, error.c_str());
        ++failures;
    }
    std::string again;
    encode(back, again);
    if (again != bytes) {
        std::printf("FAIL %s re-encodes differently\n", name);
        ++failures;
    }
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        M x;
        std::string e;
        if (decode(std::string_view(bytes).substr(0, n), x, e)) {
            std::printf("FAIL %s accepts a prefix of %zu/%zu bytes\n", name, n, bytes.size());
            ++failures;
        }
    }
    {
        M x;
        std::string e;
        if (decode(bytes + '\0', x, e) || e.empty()) {
            std::printf("FAIL %s accepts a trailing byte\n", name);
            ++failures;
        }
        std::string wrong = bytes;
        wrong[0] = static_cast<char>(wrong[0] ^ 0x40);
        if (decode(wrong, x, e)) {
            std::printf("FAIL %s accepts a wrong type byte\n", name);
            ++failures;
        }
    }
    Type t;
    CHECK(peek_type(bytes, t));
    return back;
}

void hellos_are_pinned() {
    std::string b;
    encode(EngineHello{}, b);
    CHECK(hex(b) == "01" "02000000" "01000000" "01000000");
    uint32_t w = 0;
    CHECK(decode_wire_version(b, w) && w == 2);
    Hello h;
    h.pid = 0x0102030405060708ull;
    encode(h, b);
    CHECK(hex(b) == "40" "02000000" "01000000" "01000000" "0807060504030201");
    CHECK(decode_wire_version(b, w) && w == 2);
    // A newer engine's hello with fields this version does not know: the
    // version is still readable, the message is not.
    std::string newer;
    EngineHello future;
    future.wire_version = 9;
    encode(future, newer);
    newer += "extra fields of wire 9";
    CHECK(decode_wire_version(newer, w) && w == 9);
    EngineHello x;
    std::string e;
    CHECK(!decode(newer, x, e));
    CHECK(!decode_wire_version(std::string("\x02\x01\x00\x00\x00", 5), w));  // a Load is no hello
    CHECK(!decode_wire_version(std::string("\x01\x01\x00", 3), w));
    CHECK(engine_to_host(Type::Shutdown) && !engine_to_host(Type::Hello));
    CHECK(host_to_engine(Type::Fault) && !host_to_engine(Type::Load));
}

void every_message_round_trips() {
    round_trip(EngineHello{}, "EngineHello");
    Load load;
    load.key_channels = 1;
    load.flags = kLoadReactivate;
    load.instances.push_back({Format::Lv2, "http://lv2plug.in/plugins/eg-amp", 1, ""});
    load.instances.push_back({Format::Vst3, kVstId, 3, std::string("\0state\xff", 7)});
    const Load lb = round_trip(load, "Load");
    CHECK(lb.instances.size() == 2 && lb.instances[1].state == load.instances[1].state &&
          lb.instances[1].id == kVstId && lb.flags == kLoadReactivate && lb.key_channels == 1);
    round_trip(GetState{7, 2}, "GetState");
    const DescribeValue dv = round_trip(DescribeValue{1, 2, 3, -6.5f}, "DescribeValue");
    CHECK(dv.value == -6.5f && dv.param == 3);
    round_trip(OpenEditor{4, 1, "x11:0x3a00007"}, "OpenEditor");
    round_trip(CloseEditor{1}, "CloseEditor");
    round_trip(Reconfigure{9}, "Reconfigure");
    round_trip(Shutdown{}, "Shutdown");
    round_trip(Hello{1, 1, 1, 4242}, "Hello");

    Loaded loaded;
    loaded.latency_frames = 368;
    loaded.latency_generation = 2;
    loaded.host_flags = kHostRealtime | kHostFlushDenormals;
    LoadedInstance li;
    li.latency_frames = 240;
    li.replicas = 2;
    li.state = StateResult::Restored;
    li.bypass = BypassBy::Plugin;
    li.params = {{"threshold", -60.f, 0.f, -1.f}, {"467309611", 0.f, 1.f, 0.738f}};
    loaded.instances.push_back(li);
    const Loaded lo = round_trip(loaded, "Loaded");
    CHECK(lo.instances.size() == 1 && lo.instances[0].params.size() == 2 &&
          lo.instances[0].params[1].key == "467309611" && lo.instances[0].params[0].min == -60.f &&
          lo.instances[0].state == StateResult::Restored && lo.instances[0].bypass == BypassBy::Plugin);

    round_trip(Refused{Refusal::ParamMismatch, 1, "has 3 parameters; the engine expects 4"}, "Refused");
    round_trip(State{3, 0, StateStatus::Ok, "blob", ""}, "State");
    round_trip(State{3, 0, StateStatus::Failed, "", "getState returned kResultFalse"}, "State failed");
    round_trip(ValueText{5, true, "-6.0 dB"}, "ValueText");
    round_trip(EditorOpened{6, 0}, "EditorOpened");
    round_trip(EditorFailed{6, 0, "editors are not hosted"}, "EditorFailed");
    EditorClosed ec{2, EditorCloseReason::ByOperator, {{0, 0.25f}, {kBypassParam, 1.f}}};
    const EditorClosed ecb = round_trip(ec, "EditorClosed");
    CHECK(ecb.moved.size() == 2 && ecb.moved[1].param == kBypassParam && ecb.moved[0].value == 0.25f);
    round_trip(GestureBegin{0, 3}, "GestureBegin");
    round_trip(ParamTouched{0, 3, 0.75f}, "ParamTouched");
    round_trip(GestureEnd{0, kBypassParam, 1.f}, "GestureEnd");
    round_trip(LatencyChanged{3, 1152}, "LatencyChanged");
    round_trip(Reconfigured{9, 4, 2048}, "Reconfigured");
    round_trip(Fault{FaultKind::EventsDropped, 0, 12, "touches on read-only parameters"}, "Fault");
    round_trip(Fault{FaultKind::WorkRefused, 1, 3, "worker messages refused"}, "Fault");
}

// Each field's bound, one forged message at a time.
void untrusted_fields_are_refused() {
    std::string b, e;
    const auto refuse = [&](auto msg, const char* what) {
        std::string bytes, err;
        encode(msg, bytes);
        decltype(msg) x;
        if (decode(bytes, x, err) || err.empty()) {
            std::printf("FAIL accepted: %s\n", what);
            ++failures;
        }
    };
    // Non-finite values never cross.
    refuse(ParamTouched{0, 0, std::numeric_limits<float>::quiet_NaN()}, "a NaN touch");
    refuse(GestureEnd{0, 0, std::numeric_limits<float>::infinity()}, "an Inf gesture end");
    refuse(DescribeValue{0, 0, 0, -std::numeric_limits<float>::infinity()}, "a -Inf value to describe");
    // Strings past their caps.
    refuse(Refused{Refusal::NotFound, 0, std::string(kMaxTextBytes + 1, 'x')}, "refusal text over the cap");
    refuse(OpenEditor{0, 0, std::string(kMaxTokenBytes + 1, 't')}, "a token over the cap");
    refuse(ValueText{0, true, std::string(kMaxValueTextBytes + 1, 'v')}, "value text over the cap");
    // Unknown enum values, out-of-range instances.
    refuse(Refused{static_cast<Refusal>(99), 0, ""}, "an unknown refusal");
    refuse(Refused{Refusal::NotFound, int32_t(kMaxInstances), ""}, "a refusal instance past the chain");
    refuse(Fault{FaultKind::Plugin, -2, 0, ""}, "a fault instance below -1");
    refuse(Fault{FaultKind::WorkRefused, -1, 3, ""}, "a WorkRefused fault naming no plugin");
    refuse(Fault{FaultKind::WorkRefused, 0, 0, ""}, "a WorkRefused fault counting nothing");
    refuse(Fault{static_cast<FaultKind>(4), 0, 1, ""}, "an unknown fault kind");
    refuse(State{0, 0, StateStatus::Empty, "bytes", ""}, "state bytes with status Empty");
    // Plugin ids.
    Load bad;
    bad.instances.push_back({Format::Vst3, "vst3:" + kVstId, 0, ""});
    refuse(bad, "a VST3 id with the command line's prefix");
    bad.instances[0] = {Format::Vst3, kVstId.substr(1), 0, ""};
    refuse(bad, "a VST3 id of 31 digits");
    bad.instances[0] = {Format::Lv2, "http://x y", 0, ""};
    refuse(bad, "an LV2 URI with a space");
    bad.instances[0] = {Format::Lv2, "", 0, ""};
    refuse(bad, "an empty LV2 URI");
    refuse(Load{}, "a Load with no instance");
    Load flags;
    flags.flags = 0x80;
    flags.instances.push_back({Format::Lv2, "urn:x", 0, ""});
    refuse(flags, "unknown Load flags");
    Load many;
    for (uint32_t i = 0; i <= kMaxInstances; ++i) many.instances.push_back({Format::Lv2, "urn:x", 0, ""});
    refuse(many, "more instances than a host runs");
    // Parameter tables: duplicate key, empty key, min over max.
    Loaded l;
    l.instances.push_back({});
    l.instances[0].params = {{"gain", 0, 1, 0}, {"gain", 0, 1, 0}};
    refuse(l, "a key listed twice");
    l.instances[0].params = {{"", 0, 1, 0}};
    refuse(l, "an empty key");
    l.instances[0].params = {{"gain", 2, 1, 0}};
    refuse(l, "min above max");
    l.instances[0].params = {{std::string(kMaxKeyBytes + 1, 'k'), 0, 1, 0}};
    refuse(l, "a key over the cap");
    EditorClosed twice{0, EditorCloseReason::Requested, {{1, 0.f}, {1, 1.f}}};
    refuse(twice, "a moved parameter listed twice");

    // A bool that is not 0 or 1.
    encode(ValueText{1, true, "x"}, b);
    b[5] = 2;
    ValueText vt;
    CHECK(!decode(b, vt, e));

    // Forged counts: a huge instance or param count in a short message is
    // refused before anything is reserved or read.
    encode(Loaded{}, b);  // zero instances
    {
        std::string forged = b;
        forged[13] = '\xff';
        forged[14] = '\xff';
        forged[15] = '\xff';
        forged[16] = '\x7f';
        Loaded x;
        CHECK(!decode(forged, x, e));
    }
    {
        // A string length of 4 GiB - 1 in a 20-byte message.
        std::string forged;
        encode(Refused{Refusal::None, -1, ""}, forged);
        for (std::size_t i = forged.size() - 4; i < forged.size(); ++i) forged[i] = '\xff';
        Refused x;
        CHECK(!decode(forged, x, e) && e.find("cap") != std::string::npos);
    }
    {
        // A state length just under the cap in a message that holds 3 bytes.
        std::string forged;
        encode(State{1, 0, StateStatus::Ok, "abc", ""}, forged);
        const uint32_t n = uint32_t(kMaxStateBytes);
        std::memcpy(&forged[10], &n, 4);
        State x;
        CHECK(!decode(forged, x, e) && e.find("truncated") != std::string::npos);
    }
}

// Random bytes, and random mutations of valid messages, through every
// decoder: no crash, no hang, no allocation past the message's size (the
// sanitizer job catches an overread; the caps catch the rest).
void fuzz_decoders() {
    std::mt19937 rng(20261013);
    std::vector<std::string> seeds;
    {
        std::string b;
        Load load;
        load.instances.push_back({Format::Vst3, kVstId, 2, "st"});
        encode(load, b);
        seeds.push_back(b);
        Loaded lo;
        LoadedInstance li;
        li.params = {{"a", 0, 1, 0}, {"b", -1, 1, 0}};
        lo.instances.push_back(li);
        encode(lo, b);
        seeds.push_back(b);
        encode(EditorClosed{0, EditorCloseReason::Requested, {{0, 1.f}}}, b);
        seeds.push_back(b);
        encode(State{1, 0, StateStatus::Ok, "xyz", "t"}, b);
        seeds.push_back(b);
        encode(Fault{FaultKind::Plugin, 0, 1, "f"}, b);
        seeds.push_back(b);
        encode(Hello{}, b);
        seeds.push_back(b);
    }
    std::string e;
    uint64_t accepted = 0;
    for (int iter = 0; iter < 200000; ++iter) {
        std::string b;
        if (iter % 3 == 0) {
            const std::size_t n = rng() % 64;
            for (std::size_t i = 0; i < n; ++i) b.push_back(static_cast<char>(rng()));
            if (!b.empty() && iter % 2) b[0] = static_cast<char>(1 + rng() % 8 + (rng() % 2) * 63);
        } else {
            b = seeds[rng() % seeds.size()];
            const int edits = 1 + int(rng() % 4);
            for (int k = 0; k < edits && !b.empty(); ++k) {
                switch (rng() % 3) {
                case 0: b[rng() % b.size()] = static_cast<char>(rng()); break;
                case 1: b.erase(rng() % b.size(), 1); break;
                default: b.insert(b.begin() + std::ptrdiff_t(rng() % (b.size() + 1)), static_cast<char>(rng()));
                }
            }
        }
        EngineHello m1; Load m2; GetState m3; DescribeValue m4; OpenEditor m5; CloseEditor m6;
        Reconfigure m7; Shutdown m8; Hello m9; Loaded m10; Refused m11; State m12; ValueText m13;
        EditorOpened m14; EditorFailed m15; EditorClosed m16; GestureBegin m17; ParamTouched m18;
        GestureEnd m19; LatencyChanged m20; Reconfigured m21; Fault m22; StateBlob sb;
        const bool load_ok = decode(b, m2, e), loaded_ok = decode(b, m10, e);
        accepted += decode(b, m1, e) + load_ok + decode(b, m3, e) + decode(b, m4, e) +
                    decode(b, m5, e) + decode(b, m6, e) + decode(b, m7, e) + decode(b, m8, e) +
                    decode(b, m9, e) + loaded_ok + decode(b, m11, e) + decode(b, m12, e) +
                    decode(b, m13, e) + decode(b, m14, e) + decode(b, m15, e) + decode(b, m16, e) +
                    decode(b, m17, e) + decode(b, m18, e) + decode(b, m19, e) + decode(b, m20, e) +
                    decode(b, m21, e) + decode(b, m22, e) + decode_state_blob(b, sb, e);
        // Whatever decoded honours its caps.
        if (load_ok) {
            CHECK(!m2.instances.empty() && m2.instances.size() <= kMaxInstances);
            for (const auto& i : m2.instances) CHECK(valid_plugin_id(i.format, i.id));
        }
        if (loaded_ok) CHECK(!m10.instances.empty() && m10.instances.size() <= kMaxInstances);
    }
    std::printf("  fuzz: 200000 inputs, %llu decoded as some message\n", (unsigned long long)accepted);
}

// --- frames and the Link

void frames_reassemble_and_refuse() {
    std::string e;
    std::vector<std::string> frames;
    CHECK(!split_frames("", frames, e));
    std::string big(3 * kMaxFrameBytes + 17, '\0');
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 31);
    CHECK(split_frames(big, frames, e) && frames.size() == 4);
    for (const auto& f : frames) CHECK(f.size() <= kMaxFrameBytes);
    Reassembler r;
    std::string msg;
    for (std::size_t i = 0; i + 1 < frames.size(); ++i) CHECK(r.add(frames[i], msg, e) == Reassembler::Result::Partial);
    CHECK(r.add(frames.back(), msg, e) == Reassembler::Result::Complete && msg == big && r.idle());

    // Out of place: a later frame first, a frame repeated, a length that
    // changes mid-message, a length over the cap, a frame over the cap.
    CHECK(r.add(frames[1], msg, e) == Reassembler::Result::Error && r.idle());
    CHECK(r.add(frames[0], msg, e) == Reassembler::Result::Partial);
    CHECK(r.add(frames[0], msg, e) == Reassembler::Result::Error && r.idle());
    std::string f = frames[0];
    CHECK(r.add(f, msg, e) == Reassembler::Result::Partial);
    std::string g = frames[1];
    g[0] = static_cast<char>(g[0] + 1);
    CHECK(r.add(g, msg, e) == Reassembler::Result::Error);
    std::string huge = frames[0];
    const uint32_t over = uint32_t(kMaxMessageBytes + 1);
    std::memcpy(&huge[0], &over, 4);
    CHECK(r.add(huge, msg, e) == Reassembler::Result::Error);
    CHECK(r.add(std::string(kMaxFrameBytes + 1, 'x'), msg, e) == Reassembler::Result::Error);
    CHECK(r.add(std::string(8, '\0'), msg, e) == Reassembler::Result::Error);  // no payload
    // A frame that claims a short message but carries more than it.
    std::string past;
    const uint32_t three = 3, zero = 0;
    past.append(reinterpret_cast<const char*>(&three), 4);
    past.append(reinterpret_cast<const char*>(&zero), 4);
    past += "abcd";
    CHECK(r.add(past, msg, e) == Reassembler::Result::Error);
}

// Two Links over an in-memory datagram pipe that refuses at random, as a
// full socket buffer does: every message arrives, whole, in order.
void link_over_a_blocking_transport() {
    std::mt19937 rng(7);
    std::deque<std::string> wire;
    std::size_t wire_bytes = 0;
    const std::size_t capacity = 3 * kMaxFrameBytes;
    auto send = [&](std::string_view d) {
        if (wire_bytes + d.size() > capacity || rng() % 5 == 0) return Io::WouldBlock;
        wire.emplace_back(d);
        wire_bytes += d.size();
        return Io::Ok;
    };
    auto receive = [&](std::string& d) {
        if (wire.empty()) return Io::WouldBlock;
        d = std::move(wire.front());
        wire.pop_front();
        wire_bytes -= d.size();
        return Io::Ok;
    };
    Link a, b;
    std::vector<std::string> sent, got;
    std::string e;
    for (int i = 0; i < 40; ++i) {
        State s{uint32_t(i), 0, StateStatus::Ok, std::string(std::size_t(rng() % (300 * 1024)), char('a' + i % 26)), ""};
        std::string bytes;
        encode(s, bytes);
        CHECK(a.queue(bytes, e));
        sent.push_back(bytes);
    }
    int rounds = 0;
    while ((a.wants_write() || !wire.empty()) && rounds++ < 100000) {
        const Io f = a.flush(send);
        CHECK(f == Io::Ok || f == Io::WouldBlock);
        const Io d = b.drain(receive, [&](std::string_view m) {
            got.emplace_back(m);
            return true;
        }, e, 0, 3);
        CHECK(d == Io::Ok || d == Io::WouldBlock);
    }
    CHECK(got == sent);
    CHECK(a.queued_bytes() == 0);

    // A peer that never reads: the queue refuses past its cap, and holds
    // nothing of the refused message.
    Link stuck(kMaxHostMessageBytes, 200 * 1024);
    const std::string m(100 * 1024, 'x');
    CHECK(stuck.queue(m, e));
    const std::size_t held = stuck.queued_bytes();
    CHECK(!stuck.queue(m, e) && !e.empty() && stuck.queued_bytes() == held);
    CHECK(stuck.queue("y", e));
    CHECK(!Link().queue(std::string(kMaxMessageBytes + 1, 'z'), e));

    // A handler that refuses a message ends the drain with Error.
    Link c;
    std::deque<std::string> one;
    std::vector<std::string> frames;
    split_frames("\x41", frames, e);
    one.push_back(frames[0]);
    auto recv_one = [&](std::string& d) {
        if (one.empty()) return Io::WouldBlock;
        d = one.front();
        one.pop_front();
        return Io::Ok;
    };
    CHECK(c.drain(recv_one, [&](std::string_view) {
        e = "refused";
        return false;
    }, e, 0) == Io::Error);
    // Closed is passed through.
    CHECK(c.drain([](std::string&) { return Io::Closed; }, [](std::string_view) { return true; }, e, 0) ==
          Io::Closed);
}

// rt-engineer M1 on PR #51: what one host can make the engine hold. Each
// direction has its own cap, checked at the first frame, and a message
// half received that stops growing is reported, so the engine can kill a
// host that pins memory.
static_assert(kMaxHostMessageBytes < kMaxMessageBytes, "the engine receives less than it may send");
static_assert(kMaxHostMessageBytes >= kMaxStateBytes + kMaxFrameBytes, "a State at its cap still fits");
static_assert(kMaxQueuedBytes < 2 * kMaxMessageBytes, "the engine queues one Load, not two");

std::string frame_header(uint32_t total, uint32_t offset, std::size_t payload) {
    std::string f;
    f.append(reinterpret_cast<const char*>(&total), 4);
    f.append(reinterpret_cast<const char*>(&offset), 4);
    f.append(payload, 'p');
    return f;
}

void per_direction_caps_and_stalls() {
    std::string e, msg;
    // The engine's side refuses a host message longer than its cap at the
    // first frame, holding nothing; the host's side takes a Load that long.
    Reassembler engine_side(kMaxHostMessageBytes), host_side(kMaxMessageBytes);
    const std::string over = frame_header(uint32_t(kMaxHostMessageBytes + 1), 0, 100);
    CHECK(engine_side.add(over, msg, e) == Reassembler::Result::Error && engine_side.idle() &&
          engine_side.held() == 0);
    CHECK(engine_side.add(frame_header(uint32_t(kMaxHostMessageBytes), 0, 100), msg, e) ==
          Reassembler::Result::Partial);
    CHECK(host_side.add(frame_header(uint32_t(kMaxMessageBytes), 0, 100), msg, e) == Reassembler::Result::Partial);
    CHECK(host_side.add(frame_header(uint32_t(kMaxMessageBytes + 1), 0, 100), msg, e) == Reassembler::Result::Error);
    // Link's defaults are the engine's side.
    {
        Link link;
        std::deque<std::string> in = {over};
        auto recv = [&](std::string& d) {
            if (in.empty()) return Io::WouldBlock;
            d = in.front();
            in.pop_front();
            return Io::Ok;
        };
        CHECK(link.drain(recv, [](std::string_view) { return true; }, e, 1) == Io::Error);
        CHECK(link.held_bytes() == 0);
    }

    // A stalled half message: frames stop coming. Reported once
    // kStalledMessageMs has passed since the last frame, never while idle.
    const uint64_t ms = 1000000ull, timeout = uint64_t(kStalledMessageMs) * ms;
    Link link;
    std::deque<std::string> in;
    auto recv = [&](std::string& d) {
        if (in.empty()) return Io::WouldBlock;
        d = in.front();
        in.pop_front();
        return Io::Ok;
    };
    auto ignore = [](std::string_view) { return true; };
    CHECK(!link.stalled(10 * timeout, timeout));  // idle
    in.push_back(frame_header(3 * 60000, 0, 60000));
    CHECK(link.drain(recv, ignore, e, 1000 * ms) == Io::WouldBlock);
    CHECK(!link.stalled(1000 * ms + timeout - 1, timeout));
    CHECK(link.stalled(1000 * ms + timeout, timeout));
    // A frame is progress: the clock restarts from it.
    in.push_back(frame_header(3 * 60000, 60000, 60000));
    CHECK(link.drain(recv, ignore, e, 1000 * ms + timeout) == Io::WouldBlock);
    CHECK(!link.stalled(1000 * ms + timeout + 1, timeout));
    CHECK(link.held_bytes() == 120000);
    // Complete: idle again, never stalled.
    in.push_back(frame_header(3 * 60000, 120000, 60000));
    CHECK(link.drain(recv, ignore, e, 1000 * ms + timeout + 2) == Io::WouldBlock);
    CHECK(!link.stalled(1000 * ms + 100 * timeout, timeout) && link.held_bytes() == 0);
}

// Random frame streams through the Reassembler (rt-engineer low on PR #51):
// valid messages split at random, interleaved with forged frames. Never a
// crash; never more held than the cap; every refused frame drops what was
// held; and a whole message comes out only as it went in.
void fuzz_reassembler() {
    std::mt19937 rng(51);
    const std::size_t cap = 300000;
    Reassembler r(cap);
    std::string e, msg;
    uint64_t complete = 0, errors = 0;
    for (int iter = 0; iter < 8000; ++iter) {
        std::string f;
        switch (rng() % 4) {
        case 0: {  // a whole valid message, framed
            std::string m(1 + rng() % 150000, char('a' + rng() % 26));
            std::vector<std::string> frames;
            split_frames(m, frames, e);
            bool ok = true;
            for (std::size_t k = 0; k < frames.size(); ++k) {
                const auto res = r.add(frames[k], msg, e);
                if (res == Reassembler::Result::Error) {
                    ok = false;  // a forged frame earlier left a partial
                    ++errors;
                    break;
                }
                if (res == Reassembler::Result::Complete) {
                    CHECK(k + 1 == frames.size() && msg == m);
                    ++complete;
                }
            }
            (void)ok;
            continue;
        }
        case 1:  // random bytes
            for (std::size_t n = rng() % 64; n > 0; --n) f.push_back(char(rng()));
            break;
        default:  // a header with random numbers and some payload
            f = frame_header(uint32_t(rng() % (cap * 2)), uint32_t(rng() % (cap * 2)), rng() % 70000);
        }
        const auto res = r.add(f, msg, e);
        if (res == Reassembler::Result::Error) {
            ++errors;
            CHECK(r.idle() && r.held() == 0);
        }
        CHECK(r.held() <= cap);
    }
    std::printf("  reassembler fuzz: 8000 streams, %llu messages whole, %llu frames refused\n",
                (unsigned long long)complete, (unsigned long long)errors);
    CHECK(complete > 400);
}

// --- state, files, version line

void state_blob_and_files() {
    StateBlob s{Format::Vst3, kVstId, {std::string("comp\0", 5), "ctrl"}};
    std::string bytes, e;
    CHECK(encode_state_blob(s, bytes, e));
    StateBlob back;
    CHECK(decode_state_blob(bytes, back, e) && back.format == Format::Vst3 && back.id == kVstId &&
          back.sections == s.sections);
    for (std::size_t n = 0; n < bytes.size(); ++n) CHECK(!decode_state_blob(bytes.substr(0, n), back, e));
    CHECK(!decode_state_blob(bytes + "x", back, e));
    std::string wrong = bytes;
    wrong[0] = 'X';
    CHECK(!decode_state_blob(wrong, back, e) && e.find("magic") != std::string::npos);
    StateBlob too_many{Format::Lv2, "urn:x", std::vector<std::string>(kMaxStateSections + 1)};
    CHECK(!encode_state_blob(too_many, bytes, e));
    StateBlob too_big{Format::Lv2, "urn:x", {std::string(kMaxStateBytes, 's')}};
    CHECK(!encode_state_blob(too_big, bytes, e));

    // FIPS 180-4 examples.
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(sha256_hex(std::string(1000000, 'a')) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // Lengths around the padding boundary (55, 56, 63, 64) agree with a
    // two-call split of the same bytes being impossible to get wrong: each is
    // checked against its own re-hash of a copy.
    for (std::size_t n : {55u, 56u, 63u, 64u, 65u, 119u, 120u}) {
        const std::string x(n, 'q');
        CHECK(sha256_hex(x).size() == 64 && sha256_hex(x) == sha256_hex(std::string(x)));
    }
    CHECK(sha256_hex(std::string(55, 'a')) != sha256_hex(std::string(56, 'a')));

    const std::string h = sha256_hex(bytes);
    CHECK(state_file_name(h) == "Plugin State/" + h + ".state");
    CHECK(state_file_name("ABC").empty());
    CHECK(state_file_name(std::string(64, 'G')).empty());
    CHECK(state_file_name("E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855").empty());
    CHECK(state_matches(bytes, h) && !state_matches(bytes + "x", h));
}

void version_line() {
    char buf[96];
    CHECK(format_host_version_line(buf, sizeof buf) > 0);
    CHECK(std::string(buf) == "sonora-plugin-host layout 1 clock 1 wire 2");
    uint32_t l = 0, c = 0, w = 0;
    CHECK(parse_host_version_line(buf, l, c, w) && l == kLayoutVersion && c == kClockLayoutVersion &&
          w == kHostWireVersion);
    // protocol.h's parser still reads it, skipping `wire`.
    CHECK(parse_version_line(buf, l, c) && l == 1 && c == 1);
    CHECK(parse_host_version_line("sonora-plugin-host wire 2 clock 1 layout 1 extra 9\n", l, c, w) && w == 2);
    CHECK(!parse_host_version_line("sonora-plugin-host layout 1 clock 1", l, c, w));  // a pre-wire host
    CHECK(!parse_host_version_line("sonora-plugin-host layout 1 clock 1 wire 1 wire 1", l, c, w));
    CHECK(!parse_host_version_line("sonora-plugin-host layout 1 clock 1 wire", l, c, w));
    CHECK(!parse_host_version_line("other layout 1 clock 1 wire 1", l, c, w));
    CHECK(!parse_host_version_line(nullptr, l, c, w));
}

void param_checks() {
    const uint32_t counts[2] = {3, 0};
    const ParamMap m = make_param_map(counts, 2);
    CHECK(param_in_chain(m, 0, 2) && !param_in_chain(m, 0, 3));
    CHECK(param_in_chain(m, 0, kBypassParam) && param_in_chain(m, 1, kBypassParam));
    CHECK(!param_in_chain(m, 1, 0) && !param_in_chain(m, 2, kBypassParam));
}

}  // namespace

int main() {
    hellos_are_pinned();
    every_message_round_trips();
    untrusted_fields_are_refused();
    fuzz_decoders();
    frames_reassemble_and_refuse();
    link_over_a_blocking_transport();
    per_direction_caps_and_stalls();
    fuzz_reassembler();
    state_blob_and_files();
    version_line();
    param_checks();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("host_control: all checks passed\n");
    return 0;
}
