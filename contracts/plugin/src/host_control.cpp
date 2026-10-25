// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/plugin/host_control.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <utility>

namespace sonora::plugin::control {

namespace {

// --- writing

void put_u8(std::string& out, uint8_t v) { out.push_back(static_cast<char>(v)); }

void put_u32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void put_u64(std::string& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void put_i32(std::string& out, int32_t v) { put_u32(out, static_cast<uint32_t>(v)); }

void put_f32(std::string& out, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    put_u32(out, bits);
}

void put_bool(std::string& out, bool v) { put_u8(out, v ? 1 : 0); }

void put_bytes(std::string& out, std::string_view s) {
    put_u32(out, static_cast<uint32_t>(s.size()));
    out.append(s.data(), s.size());
}

void put_type(std::string& out, Type t) {
    out.clear();
    put_u8(out, static_cast<uint8_t>(t));
}

// --- reading. Every read is bounds-checked; the first failure sticks and
// names the field, so a decoder reads straight through and checks once.

class Reader {
public:
    Reader(std::string_view b, std::string& error) : b_(b), error_(error) {}

    bool expect_type(Type t) {
        uint8_t v = 0;
        if (!u8(v, "type")) return false;
        if (v != static_cast<uint8_t>(t))
            return fail("type byte " + std::to_string(v) + ", not " +
                        std::to_string(static_cast<unsigned>(t)));
        return true;
    }
    bool u8(uint8_t& v, const char* what) {
        if (!need(1, what)) return false;
        v = static_cast<uint8_t>(b_[at_++]);
        return true;
    }
    bool u32(uint32_t& v, const char* what) {
        if (!need(4, what)) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(static_cast<uint8_t>(b_[at_ + i])) << (8 * i);
        at_ += 4;
        return true;
    }
    bool u64(uint64_t& v, const char* what) {
        if (!need(8, what)) return false;
        v = 0;
        for (int i = 0; i < 8; ++i) v |= uint64_t(static_cast<uint8_t>(b_[at_ + i])) << (8 * i);
        at_ += 8;
        return true;
    }
    bool i32(int32_t& v, const char* what) {
        uint32_t u = 0;
        if (!u32(u, what)) return false;
        v = static_cast<int32_t>(u);
        return true;
    }
    // Finite only: no message carries a NaN or an Inf.
    bool f32(float& v, const char* what) {
        uint32_t bits = 0;
        if (!u32(bits, what)) return false;
        if ((bits & 0x7F800000u) == 0x7F800000u) return fail(std::string(what) + " is not finite");
        std::memcpy(&v, &bits, sizeof v);
        return true;
    }
    bool boolean(bool& v, const char* what) {
        uint8_t u = 0;
        if (!u8(u, what)) return false;
        if (u > 1) return fail(std::string(what) + " is not a bool");
        v = u == 1;
        return true;
    }
    // The length is checked against the cap before anything is copied.
    bool bytes(std::string& v, std::size_t cap, const char* what) {
        uint32_t n = 0;
        if (!u32(n, what)) return false;
        if (n > cap)
            return fail(std::string(what) + " of " + std::to_string(n) + " bytes, over the cap of " +
                        std::to_string(cap));
        if (!need(n, what)) return false;
        v.assign(b_.data() + at_, n);
        at_ += n;
        return true;
    }
    // A count the remaining bytes could hold at `min_each` bytes per entry,
    // and at most `cap`: a forged count can never make a decoder reserve.
    bool count(uint32_t& n, uint32_t cap, std::size_t min_each, const char* what) {
        if (!u32(n, what)) return false;
        if (n > cap)
            return fail(std::string(what) + " " + std::to_string(n) + " is over the cap of " +
                        std::to_string(cap));
        if (min_each && std::size_t(n) > (b_.size() - at_) / min_each)
            return fail(std::string(what) + " " + std::to_string(n) + " cannot fit what is left");
        return true;
    }
    template <class E>
    bool enumerated(E& v, uint8_t last, const char* what) {
        uint8_t u = 0;
        if (!u8(u, what)) return false;
        if (u > last) return fail(std::string(what) + " " + std::to_string(u) + " is unknown");
        v = static_cast<E>(u);
        return true;
    }
    bool done() {
        if (failed_) return false;
        if (at_ != b_.size())
            return fail(std::to_string(b_.size() - at_) + " trailing byte(s)");
        return true;
    }
    bool fail(std::string why) {
        if (!failed_) error_ = std::move(why);
        failed_ = true;
        return false;
    }
    bool ok() const { return !failed_; }

private:
    bool need(std::size_t n, const char* what) {
        if (failed_) return false;
        if (b_.size() - at_ < n) return fail(std::string("truncated at ") + what);
        return true;
    }

    std::string_view b_;
    std::string& error_;
    std::size_t at_ = 0;
    bool failed_ = false;
};

bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

}  // namespace

// --- ids --------------------------------------------------------------------

bool valid_plugin_id(Format format, std::string_view id) {
    if (format == Format::Vst3) {
        if (id.size() != 32) return false;
        for (char c : id)
            if (!is_hex(c)) return false;
        return true;
    }
    if (id.empty() || id.size() > kMaxIdBytes) return false;
    for (unsigned char c : id)
        if (c <= 0x20 || c == 0x7F) return false;
    return true;
}

// --- peek -------------------------------------------------------------------

bool peek_type(std::string_view bytes, Type& out) {
    if (bytes.empty()) return false;
    const auto t = static_cast<Type>(static_cast<uint8_t>(bytes[0]));
    if (!engine_to_host(t) && !host_to_engine(t)) return false;
    out = t;
    return true;
}

bool decode_wire_version(std::string_view bytes, uint32_t& wire_version) {
    if (bytes.size() < 5) return false;
    const auto t = static_cast<uint8_t>(bytes[0]);
    if (t != static_cast<uint8_t>(Type::EngineHello) && t != static_cast<uint8_t>(Type::Hello)) return false;
    wire_version = 0;
    for (int i = 0; i < 4; ++i) wire_version |= uint32_t(static_cast<uint8_t>(bytes[1 + i])) << (8 * i);
    return true;
}

// --- engine -> host ---------------------------------------------------------

void encode(const EngineHello& m, std::string& out) {
    put_type(out, Type::EngineHello);
    put_u32(out, m.wire_version);
    put_u32(out, m.layout_version);
    put_u32(out, m.clock_version);
}

bool decode(std::string_view b, EngineHello& m, std::string& error) {
    Reader r(b, error);
    r.expect_type(Type::EngineHello);
    r.u32(m.wire_version, "wire_version");
    r.u32(m.layout_version, "layout_version");
    r.u32(m.clock_version, "clock_version");
    return r.done();
}

void encode(const Load& m, std::string& out) {
    put_type(out, Type::Load);
    put_u32(out, m.key_channels);
    put_u32(out, m.flags);
    put_u32(out, static_cast<uint32_t>(m.instances.size()));
    for (const LoadInstance& i : m.instances) {
        put_u8(out, static_cast<uint8_t>(i.format));
        put_bytes(out, i.id);
        put_u32(out, i.params);
        put_bytes(out, i.state);
    }
}

bool decode(std::string_view b, Load& m, std::string& error) {
    Reader r(b, error);
    r.expect_type(Type::Load);
    r.u32(m.key_channels, "key_channels");
    r.u32(m.flags, "flags");
    if (r.ok() && (m.flags & ~kLoadKnownFlags)) r.fail("flags " + std::to_string(m.flags) + " has unknown bits");
    uint32_t n = 0;
    r.count(n, kMaxInstances, 13, "instance count");
    if (r.ok() && n == 0) r.fail("a Load with no instance");
    m.instances.clear();
    for (uint32_t k = 0; k < n && r.ok(); ++k) {
        LoadInstance i;
        r.enumerated(i.format, static_cast<uint8_t>(Format::Vst3), "format");
        r.bytes(i.id, kMaxIdBytes, "id");
        r.u32(i.params, "params");
        r.bytes(i.state, kMaxStateBytes, "state");
        if (r.ok() && !valid_plugin_id(i.format, i.id)) r.fail("instance " + std::to_string(k) + ": not a plugin id");
        m.instances.push_back(std::move(i));
    }
    return r.done();
}

void encode(const GetState& m, std::string& out) {
    put_type(out, Type::GetState);
    put_u32(out, m.request);
    put_u32(out, m.instance);
}

bool decode(std::string_view b, GetState& m, std::string& error) {
    Reader r(b, error);
    r.expect_type(Type::GetState);
    r.u32(m.request, "request");
    r.u32(m.instance, "instance");
    return r.done();
}

void encode(const DescribeValue& m, std::string& out) {
    put_type(out, Type::DescribeValue);
    put_u32(out, m.request);
    put_u32(out, m.instance);
    put_u32(out, m.param);
    put_f32(out, m.value);
}

bool decode(std::string_view b, DescribeValue& m, std::string& error) {
    Reader r(b, error);
    r.expect_type(Type::DescribeValue);
    r.u32(m.request, "request");
    r.u32(m.instance, "instance");
    r.u32(m.param, "param");
    r.f32(m.value, "value");
    return r.done();
}

void encode(const OpenEditor& m, std::string& out) {
    put_type(out, Type::OpenEditor);
    put_u32(out, m.request);
    put_u32(out, m.instance);
    put_bytes(out, m.parent_token);
}

bool decode(std::string_view b, OpenEditor& m, std::string& error) {
    Reader r(b, error);
    r.expect_type(Type::OpenEditor);
    r.u32(m.request, "request");
    r.u32(m.instance, "instance");
    r.bytes(m.parent_token, kMaxTokenBytes, "parent_token");
    return r.done();
}

void encode(const CloseEditor& m, std::string& out) {
    put_type(out, Type::CloseEditor);
    put_u32(out, m.instance);
}

bool decode(std::string_view b, CloseEditor& m, std::string& error) {
    Reader r(b, error);
    r.expect_type(Type::CloseEditor);
    r.u32(m.instance, "instance");
    return r.done();
}

void encode(const Reconfigure& m, std::string& out) {
    put_type(out, Type::Reconfigure);
    put_u32(out, m.request);
}

bool decode(std::string_view b, Reconfigure& m, std::string& error) {
    Reader r(b, error);
    r.expect_type(Type::Reconfigure);
    r.u32(m.request, "request");
    return r.done();
}

void encode(const Shutdown&, std::string& out) { put_type(out, Type::Shutdown); }

bool decode(std::string_view b, Shutdown&, std::string& error) {
    Reader r(b, error);
    r.expect_type(Type::Shutdown);
    return r.done();
}

// --- host -> engine ---------------------------------------------------------

void encode(const Hello& m, std::string& out) {
    put_type(out, Type::Hello);
    put_u32(out, m.wire_version);
    put_u32(out, m.layout_version);
    put_u32(out, m.clock_version);
    put_u64(out, m.pid);
}

bool decode(std::string_view b, Hello& m, std::string& error) {
    Reader r(b, error);
    r.expect_type(Type::Hello);
    r.u32(m.wire_version, "wire_version");
    r.u32(m.layout_version, "layout_version");
    r.u32(m.clock_version, "clock_version");
    r.u64(m.pid, "pid");
    return r.done();
}

void encode(const Loaded& m, std::string& out) {
    put_type(out, Type::Loaded);
    put_u32(out, m.latency_frames);
    put_u32(out, m.latency_generation);
    put_u32(out, m.host_flags);
    put_u32(out, static_cast<uint32_t>(m.instances.size()));
    for (const LoadedInstance& i : m.instances) {
        put_u32(out, i.latency_frames);
        put_u32(out, i.replicas);
        put_u8(out, static_cast<uint8_t>(i.state));
        put_u8(out, static_cast<uint8_t>(i.bypass));
        put_u32(out, static_cast<uint32_t>(i.params.size()));
        for (const ParamDesc& p : i.params) {
            put_bytes(out, p.key);
            put_f32(out, p.min);
            put_f32(out, p.max);
            put_f32(out, p.default_value);
        }
    }
}

bool decode(std::string_view b, Loaded& m, std::string& error) {
    Reader r(b, error);
    r.expect_type(Type::Loaded);
    r.u32(m.latency_frames, "latency_frames");
    r.u32(m.latency_generation, "latency_generation");
    r.u32(m.host_flags, "host_flags");
    uint32_t n = 0;
    r.count(n, kMaxInstances, 14, "instance count");
    if (r.ok() && n == 0) r.fail("Loaded with no instance");
    m.instances.clear();
    for (uint32_t k = 0; k < n && r.ok(); ++k) {
        LoadedInstance i;
        r.u32(i.latency_frames, "instance latency_frames");
        r.u32(i.replicas, "replicas");
        r.enumerated(i.state, static_cast<uint8_t>(StateResult::Refused), "state result");
        r.enumerated(i.bypass, static_cast<uint8_t>(BypassBy::Plugin), "bypass");
        uint32_t np = 0;
        r.count(np, kMaxParamsPerInstance, 17, "param count");
        std::set<std::string> keys;
        for (uint32_t p = 0; p < np && r.ok(); ++p) {
            ParamDesc d;
            r.bytes(d.key, kMaxKeyBytes, "key");
            r.f32(d.min, "min");
            r.f32(d.max, "max");
            r.f32(d.default_value, "default");
            if (!r.ok()) break;
            if (d.key.empty()) r.fail("an empty parameter key");
            else if (!(d.min <= d.max)) r.fail("parameter " + d.key + ": min above max");
            else if (!keys.insert(d.key).second) r.fail("parameter key " + d.key + " twice");
            i.params.push_back(std::move(d));
        }
        m.instances.push_back(std::move(i));
    }
    return r.done();
}

void encode(const Refused& m, std::string& out) {
    put_type(out, Type::Refused);
    put_u32(out, static_cast<uint32_t>(m.reason));
    put_i32(out, m.instance);
    put_bytes(out, m.text);
}

bool decode(std::string_view b, Refused& m, std::string& error) {
    Reader r(b, error);
    r.expect_type(Type::Refused);
    uint32_t reason = 0;
    r.u32(reason, "reason");
    if (r.ok() && reason > static_cast<uint32_t>(Refusal::ParamMismatch))
        r.fail("refusal " + std::to_string(reason) + " is unknown");
    m.reason = static_cast<Refusal>(reason);
    r.i32(m.instance, "instance");
    if (r.ok() && (m.instance < -1 || m.instance >= int32_t(kMaxInstances)))
        r.fail("refusal instance " + std::to_string(m.instance) + " is out of range");
    r.bytes(m.text, kMaxTextBytes, "text");
    return r.done();
}

void encode(const State& m, std::string& out) {
    put_type(out, Type::State);
    put_u32(out, m.request);
    put_u32(out, m.instance);
    put_u8(out, static_cast<uint8_t>(m.status));
    put_bytes(out, m.bytes);
    put_bytes(out, m.text);
}

bool decode(std::string_view b, State& m, std::string& error) {
    Reader r(b, error);
    r.expect_type(Type::State);
    r.u32(m.request, "request");
    r.u32(m.instance, "instance");
    r.enumerated(m.status, static_cast<uint8_t>(StateStatus::Failed), "status");
    r.bytes(m.bytes, kMaxStateBytes, "bytes");
    r.bytes(m.text, kMaxTextBytes, "text");
    if (r.ok() && m.status != StateStatus::Ok && !m.bytes.empty()) r.fail("state bytes with a status not Ok");
    return r.done();
}

void encode(const ValueText& m, std::string& out) {
    put_type(out, Type::ValueText);
    put_u32(out, m.request);
    put_bool(out, m.ok);
    put_bytes(out, m.text);
}

bool decode(std::string_view b, ValueText& m, std::string& error) {
    Reader r(b, error);
    r.expect_type(Type::ValueText);
    r.u32(m.request, "request");
    r.boolean(m.ok, "ok");
    r.bytes(m.text, kMaxValueTextBytes, "text");
    return r.done();
}

void encode(const EditorOpened& m, std::string& out) {
    put_type(out, Type::EditorOpened);
    put_u32(out, m.request);
    put_u32(out, m.instance);
}

bool decode(std::string_view b, EditorOpened& m, std::string& error) {
    Reader r(b, error);
    r.expect_type(Type::EditorOpened);
    r.u32(m.request, "request");
    r.u32(m.instance, "instance");
    return r.done();
}

void encode(const EditorFailed& m, std::string& out) {
    put_type(out, Type::EditorFailed);
    put_u32(out, m.request);
    put_u32(out, m.instance);
    put_bytes(out, m.text);
}

bool decode(std::string_view b, EditorFailed& m, std::string& error) {
    Reader r(b, error);
    r.expect_type(Type::EditorFailed);
    r.u32(m.request, "request");
    r.u32(m.instance, "instance");
    r.bytes(m.text, kMaxTextBytes, "text");
    return r.done();
}

void encode(const EditorClosed& m, std::string& out) {
    put_type(out, Type::EditorClosed);
    put_u32(out, m.instance);
    put_u8(out, static_cast<uint8_t>(m.reason));
    put_u32(out, static_cast<uint32_t>(m.moved.size()));
    for (const TouchedValue& t : m.moved) {
        put_u32(out, t.param);
        put_f32(out, t.value);
    }
}

bool decode(std::string_view b, EditorClosed& m, std::string& error) {
    Reader r(b, error);
    r.expect_type(Type::EditorClosed);
    r.u32(m.instance, "instance");
    r.enumerated(m.reason, static_cast<uint8_t>(EditorCloseReason::HostStopping), "reason");
    uint32_t n = 0;
    r.count(n, kMaxParamsPerInstance + 1, 8, "moved count");
    m.moved.clear();
    std::set<uint32_t> seen;
    for (uint32_t k = 0; k < n && r.ok(); ++k) {
        TouchedValue t;
        r.u32(t.param, "moved param");
        r.f32(t.value, "moved value");
        if (r.ok() && !seen.insert(t.param).second) r.fail("moved param " + std::to_string(t.param) + " twice");
        m.moved.push_back(t);
    }
    return r.done();
}

namespace {

template <class M>
void encode_touch(Type t, const M& m, std::string& out, bool with_value) {
    put_type(out, t);
    put_u32(out, m.instance);
    put_u32(out, m.param);
    if (with_value) put_f32(out, m.value);
}

}  // namespace

void encode(const GestureBegin& m, std::string& out) {
    put_type(out, Type::GestureBegin);
    put_u32(out, m.instance);
    put_u32(out, m.param);
}

bool decode(std::string_view b, GestureBegin& m, std::string& error) {
    Reader r(b, error);
    r.expect_type(Type::GestureBegin);
    r.u32(m.instance, "instance");
    r.u32(m.param, "param");
    return r.done();
}

void encode(const ParamTouched& m, std::string& out) { encode_touch(Type::ParamTouched, m, out, true); }

bool decode(std::string_view b, ParamTouched& m, std::string& error) {
    Reader r(b, error);
    r.expect_type(Type::ParamTouched);
    r.u32(m.instance, "instance");
    r.u32(m.param, "param");
    r.f32(m.value, "value");
    return r.done();
}

void encode(const GestureEnd& m, std::string& out) { encode_touch(Type::GestureEnd, m, out, true); }

bool decode(std::string_view b, GestureEnd& m, std::string& error) {
    Reader r(b, error);
    r.expect_type(Type::GestureEnd);
    r.u32(m.instance, "instance");
    r.u32(m.param, "param");
    r.f32(m.value, "value");
    return r.done();
}

void encode(const LatencyChanged& m, std::string& out) {
    put_type(out, Type::LatencyChanged);
    put_u32(out, m.latency_generation);
    put_u32(out, m.latency_frames);
}

bool decode(std::string_view b, LatencyChanged& m, std::string& error) {
    Reader r(b, error);
    r.expect_type(Type::LatencyChanged);
    r.u32(m.latency_generation, "latency_generation");
    r.u32(m.latency_frames, "latency_frames");
    return r.done();
}

void encode(const Reconfigured& m, std::string& out) {
    put_type(out, Type::Reconfigured);
    put_u32(out, m.request);
    put_u32(out, m.latency_generation);
    put_u32(out, m.latency_frames);
}

bool decode(std::string_view b, Reconfigured& m, std::string& error) {
    Reader r(b, error);
    r.expect_type(Type::Reconfigured);
    r.u32(m.request, "request");
    r.u32(m.latency_generation, "latency_generation");
    r.u32(m.latency_frames, "latency_frames");
    return r.done();
}

void encode(const Fault& m, std::string& out) {
    put_type(out, Type::Fault);
    put_u8(out, static_cast<uint8_t>(m.kind));
    put_i32(out, m.instance);
    put_u64(out, m.count);
    put_bytes(out, m.text);
}

bool decode(std::string_view b, Fault& m, std::string& error) {
    Reader r(b, error);
    r.expect_type(Type::Fault);
    r.enumerated(m.kind, static_cast<uint8_t>(FaultKind::WorkRefused), "kind");
    r.i32(m.instance, "instance");
    if (r.ok() && (m.instance < -1 || m.instance >= int32_t(kMaxInstances)))
        r.fail("fault instance " + std::to_string(m.instance) + " is out of range");
    r.u64(m.count, "count");
    // It names a plugin, and says something was refused.
    if (r.ok() && m.kind == FaultKind::WorkRefused && (m.instance < 0 || m.count == 0))
        r.fail("a WorkRefused fault names no instance or counts nothing");
    r.bytes(m.text, kMaxTextBytes, "text");
    return r.done();
}

// --- frames -----------------------------------------------------------------

namespace {

uint32_t read_u32(std::string_view b, std::size_t at) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t(static_cast<uint8_t>(b[at + i])) << (8 * i);
    return v;
}

}  // namespace

bool split_frames(std::string_view message, std::vector<std::string>& frames, std::string& error) {
    if (message.empty()) {
        error = "an empty message";
        return false;
    }
    if (message.size() > kMaxMessageBytes) {
        error = "a message of " + std::to_string(message.size()) + " bytes, over the cap of " +
                std::to_string(kMaxMessageBytes);
        return false;
    }
    constexpr std::size_t payload = kMaxFrameBytes - kFrameHeaderBytes;
    for (std::size_t at = 0; at < message.size(); at += payload) {
        const std::size_t n = std::min(payload, message.size() - at);
        std::string f;
        f.reserve(kFrameHeaderBytes + n);
        put_u32(f, static_cast<uint32_t>(message.size()));
        put_u32(f, static_cast<uint32_t>(at));
        f.append(message.data() + at, n);
        frames.push_back(std::move(f));
    }
    return true;
}

Reassembler::Result Reassembler::fail(std::string& error, std::string why) {
    error = std::move(why);
    partial_.clear();
    partial_.shrink_to_fit();
    expected_ = 0;
    return Result::Error;
}

Reassembler::Result Reassembler::add(std::string_view frame, std::string& message, std::string& error) {
    if (frame.size() > kMaxFrameBytes)
        return fail(error, "a frame of " + std::to_string(frame.size()) + " bytes, over " +
                               std::to_string(kMaxFrameBytes));
    if (frame.size() <= kFrameHeaderBytes) return fail(error, "a frame with no payload");
    const uint32_t total = read_u32(frame, 0);
    const uint32_t offset = read_u32(frame, 4);
    const std::string_view body = frame.substr(kFrameHeaderBytes);
    if (total == 0 || total > max_)
        return fail(error, "a message length of " + std::to_string(total) + " bytes, over this direction's cap of " +
                               std::to_string(max_));
    if (expected_ == 0) {
        if (offset != 0) return fail(error, "a message that does not start at offset 0");
        expected_ = total;
        partial_.clear();
    } else if (total != expected_ || offset != partial_.size()) {
        return fail(error, "a frame out of place (length " + std::to_string(total) + ", offset " +
                               std::to_string(offset) + "; expected " + std::to_string(expected_) +
                               " at " + std::to_string(partial_.size()) + ")");
    }
    if (body.size() > expected_ - partial_.size()) return fail(error, "a frame past its message's end");
    partial_.append(body.data(), body.size());
    if (partial_.size() < expected_) return Result::Partial;
    message.swap(partial_);
    partial_.clear();
    expected_ = 0;
    return Result::Complete;
}

bool Link::queue(std::string_view message, std::string& error) {
    std::vector<std::string> frames;
    if (!split_frames(message, frames, error)) return false;
    std::size_t bytes = 0;
    for (const std::string& f : frames) bytes += f.size();
    if (queued_bytes_ + bytes > max_queued_) {
        error = "the peer is not reading: " + std::to_string(queued_bytes_) + " bytes already wait to be sent";
        return false;
    }
    for (std::string& f : frames) out_.push_back(std::move(f));
    queued_bytes_ += bytes;
    return true;
}

// --- state ------------------------------------------------------------------

bool encode_state_blob(const StateBlob& blob, std::string& out, std::string& error) {
    if (blob.sections.size() > kMaxStateSections) {
        error = std::to_string(blob.sections.size()) + " state sections, over " + std::to_string(kMaxStateSections);
        return false;
    }
    if (!valid_plugin_id(blob.format, blob.id)) {
        error = "not a plugin id: " + blob.id;
        return false;
    }
    out.clear();
    put_u32(out, kStateMagic);
    put_u32(out, kStateBlobVersion);
    put_u8(out, static_cast<uint8_t>(blob.format));
    put_bytes(out, blob.id);
    put_u32(out, static_cast<uint32_t>(blob.sections.size()));
    for (const std::string& s : blob.sections) {
        if (out.size() + 4 + s.size() > kMaxStateBytes) {
            error = "the state is over " + std::to_string(kMaxStateBytes) + " bytes";
            out.clear();
            return false;
        }
        put_bytes(out, s);
    }
    return true;
}

bool decode_state_blob(std::string_view bytes, StateBlob& out, std::string& error) {
    if (bytes.size() > kMaxStateBytes) {
        error = "a state of " + std::to_string(bytes.size()) + " bytes, over the cap";
        return false;
    }
    Reader r(bytes, error);
    uint32_t magic = 0, version = 0;
    r.u32(magic, "magic");
    if (r.ok() && magic != kStateMagic) r.fail("not a Sonora plugin state (magic)");
    r.u32(version, "state version");
    if (r.ok() && version != kStateBlobVersion)
        r.fail("state version " + std::to_string(version) + "; this build reads " + std::to_string(kStateBlobVersion));
    r.enumerated(out.format, static_cast<uint8_t>(Format::Vst3), "format");
    r.bytes(out.id, kMaxIdBytes, "id");
    if (r.ok() && !valid_plugin_id(out.format, out.id)) r.fail("not a plugin id");
    uint32_t n = 0;
    r.count(n, uint32_t(kMaxStateSections), 4, "section count");
    out.sections.clear();
    for (uint32_t k = 0; k < n && r.ok(); ++k) {
        std::string s;
        r.bytes(s, kMaxStateBytes, "section");
        out.sections.push_back(std::move(s));
    }
    return r.done();
}

// SHA-256, FIPS 180-4. Here rather than a dependency: contracts/plugin
// depends on common/base only, and this is what names a state file.
std::string sha256_hex(std::string_view bytes) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    const auto block = [&](const unsigned char* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 |
                   uint32_t(p[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    };
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    std::size_t n = bytes.size(), at = 0;
    for (; n - at >= 64; at += 64) block(data + at);
    unsigned char tail[128] = {};
    const std::size_t rest = n - at;
    std::memcpy(tail, data + at, rest);
    tail[rest] = 0x80;
    const std::size_t tail_len = rest + 1 + 8 <= 64 ? 64 : 128;
    const uint64_t bits = uint64_t(n) * 8;
    for (int i = 0; i < 8; ++i) tail[tail_len - 1 - i] = static_cast<unsigned char>(bits >> (8 * i));
    block(tail);
    if (tail_len == 128) block(tail + 64);
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (uint32_t v : h)
        for (int i = 28; i >= 0; i -= 4) out.push_back(hex[(v >> i) & 0xF]);
    return out;
}

std::string state_file_name(std::string_view sha256) {
    if (sha256.size() != 64) return {};
    for (char c : sha256)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return {};
    return "Plugin State/" + std::string(sha256) + ".state";
}

bool state_matches(std::string_view bytes, std::string_view sha256) { return sha256_hex(bytes) == sha256; }

// --- --version ----------------------------------------------------------------

int format_host_version_line(char* out, std::size_t size) {
    return std::snprintf(out, size, "%s layout %u clock %u wire %u", kVersionProgram, kLayoutVersion,
                         kClockLayoutVersion, kHostWireVersion);
}

bool parse_host_version_line(const char* line, uint32_t& layout, uint32_t& clock, uint32_t& wire) {
    const std::size_t n = std::strlen(kVersionProgram);
    if (!line || std::strncmp(line, kVersionProgram, n) != 0) return false;
    const char* p = line + n;
    bool have[3] = {false, false, false};
    uint32_t* const into[3] = {&layout, &clock, &wire};
    static const char* const names[3] = {"layout", "clock", "wire"};
    while (*p == ' ') {
        ++p;
        const char* key = p;
        while (*p >= 'a' && *p <= 'z') ++p;
        const std::size_t key_len = std::size_t(p - key);
        if (key_len == 0 || *p != ' ') return false;
        ++p;
        if (*p < '0' || *p > '9') return false;
        uint64_t v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + uint64_t(*p - '0');
            if (v > 0xFFFFFFFFu) return false;
            ++p;
        }
        for (int i = 0; i < 3; ++i) {
            if (key_len == std::strlen(names[i]) && std::strncmp(key, names[i], key_len) == 0) {
                if (have[i]) return false;
                have[i] = true;
                *into[i] = uint32_t(v);
            }
        }
    }
    if (*p == '\n') ++p;
    return *p == '\0' && have[0] && have[1] && have[2];
}

}  // namespace sonora::plugin::control
