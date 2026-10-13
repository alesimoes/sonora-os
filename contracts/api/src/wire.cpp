// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/api/wire.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "sonora/api/state_block.h"

namespace sonora::api::wire {

namespace {

class Writer {
public:
    explicit Writer(std::string& out, Type type) : out_(out) {
        out_.clear();
        out_.push_back(static_cast<char>(type));
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) out_.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) out_.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void f32(float v) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        u32(bits);
    }
    void str(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        out_.append(s);
    }
    void list(const std::vector<std::string>& v) {
        u32(static_cast<uint32_t>(v.size()));
        for (const auto& s : v) str(s);
    }

private:
    std::string& out_;
};

class Reader {
public:
    Reader(std::string_view in, std::string& error) : in_(in), error_(error) {}

    bool type(Type expected) {
        if (in_.empty()) return fail("empty message");
        if (static_cast<uint8_t>(in_[0]) != static_cast<uint8_t>(expected)) {
            return fail("message type " + std::to_string(static_cast<uint8_t>(in_[0])) +
                        " where type " + std::to_string(static_cast<uint8_t>(expected)) +
                        " was expected");
        }
        pos_ = 1;
        return true;
    }
    bool u32(uint32_t& v) {
        if (!need(4)) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<uint32_t>(static_cast<uint8_t>(in_[pos_ + i])) << (8 * i);
        }
        pos_ += 4;
        return true;
    }
    bool u64(uint64_t& v) {
        if (!need(8)) return false;
        v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<uint64_t>(static_cast<uint8_t>(in_[pos_ + i])) << (8 * i);
        }
        pos_ += 8;
        return true;
    }
    bool i32(int32_t& v) {
        uint32_t u = 0;
        if (!u32(u)) return false;
        v = static_cast<int32_t>(u);
        return true;
    }
    bool f32(float& v) {
        uint32_t bits = 0;
        if (!u32(bits)) return false;
        std::memcpy(&v, &bits, sizeof v);
        return true;
    }
    bool str(std::string& s, std::size_t cap = kMaxText) {
        uint32_t n = 0;
        if (!u32(n)) return false;
        if (n > cap) {
            return fail("a string of " + std::to_string(n) + " bytes, over the cap of " +
                        std::to_string(cap));
        }
        if (!need(n)) return false;
        s.assign(in_.data() + pos_, n);
        pos_ += n;
        return true;
    }
    bool list(std::vector<std::string>& v, std::size_t cap = kMaxListEntries) {
        uint32_t n = 0;
        if (!u32(n)) return false;
        if (n > cap) {
            return fail("a list of " + std::to_string(n) + " entries, over the cap of " +
                        std::to_string(cap));
        }
        v.clear();
        for (uint32_t i = 0; i < n; ++i) {
            std::string s;
            if (!str(s)) return false;
            v.push_back(std::move(s));
        }
        return true;
    }
    // A bool is a u32 that is 0 or 1; anything else is a peer that does not
    // mean what it says.
    bool flag(bool& out) {
        uint32_t v = 0;
        if (!u32(v)) return false;
        if (v > 1) return fail("a flag of " + std::to_string(v) + ", where 0 or 1 was expected");
        out = v == 1;
        return true;
    }
    // A count of entries to follow, refused past `cap` before anything is
    // reserved for them.
    bool count(uint32_t& n, std::size_t cap, const char* what) {
        if (!u32(n)) return false;
        if (n > cap) {
            return fail(std::string("a list of ") + std::to_string(n) + " " + what +
                        ", over the cap of " + std::to_string(cap));
        }
        return true;
    }
    template <typename E>
    bool enumeration(E& out, uint32_t last) {
        uint32_t v = 0;
        if (!u32(v)) return false;
        if (v > last) return fail("enum value " + std::to_string(v) + " is unknown here");
        out = static_cast<E>(v);
        return true;
    }
    bool done() {
        if (pos_ != in_.size()) {
            return fail(std::to_string(in_.size() - pos_) + " bytes left over");
        }
        return true;
    }
    bool fail(const std::string& why) {
        error_ = why;
        return false;
    }

private:
    bool need(std::size_t n) {
        if (in_.size() - pos_ < n) return fail("message cut short");
        return true;
    }

    std::string_view in_;
    std::string& error_;
    std::size_t pos_ = 0;
};

constexpr uint32_t kLastDispatchResult = static_cast<uint32_t>(DispatchResult::Failed);
constexpr uint32_t kLastParam = static_cast<uint32_t>(ParamId::Gain);
constexpr uint32_t kLastAction = static_cast<uint32_t>(ModelAction::FlushPluginState);
constexpr uint32_t kLastEvent = static_cast<uint32_t>(EventKind::PluginFault);
constexpr uint32_t kLastUpdate = static_cast<uint32_t>(ModelUpdateKind::Snapshot);
constexpr uint32_t kLastFormat = static_cast<uint32_t>(PluginFormat::Vst3);

}  // namespace

bool peek_type(std::string_view bytes, Type& out) {
    if (bytes.empty()) return false;
    out = static_cast<Type>(static_cast<uint8_t>(bytes[0]));
    return true;
}

void encode(const Hello& m, std::string& out) {
    Writer w(out, Type::Hello);
    w.u32(m.protocol_version);
    w.u32(m.state_layout_version);
    w.u32(m.sample_rate);
    w.u32(m.buffer_frames);
    w.u32(m.track_count);
    w.list(m.track_names);
    w.list(m.implemented_commands);
}

void encode(const CommandRequest& m, std::string& out) {
    Writer w(out, Type::CommandRequest);
    w.u64(m.seq);
    w.str(m.command);
    w.i32(m.target);
    w.str(m.argument);
}

void encode(const ParameterChange& m, std::string& out) {
    Writer w(out, Type::ParameterChange);
    w.u64(m.seq);
    w.u32(m.track);
    w.u32(static_cast<uint32_t>(m.param));
    w.f32(m.value);
}

void encode(const ModelRequest& m, std::string& out) {
    Writer w(out, Type::ModelRequest);
    w.u64(m.seq);
    w.u32(static_cast<uint32_t>(m.action));
    w.str(m.op);
}

void encode(const CommandReply& m, std::string& out) {
    Writer w(out, Type::CommandReply);
    w.u64(m.seq);
    w.u32(static_cast<uint32_t>(m.result));
    w.str(m.message);
}

void encode(const EngineEvent& m, std::string& out) {
    Writer w(out, Type::EngineEvent);
    w.u32(static_cast<uint32_t>(m.kind));
    w.i32(m.track);
    w.u64(m.frame);
    w.u64(m.insert);
    w.str(m.text);
}

void encode(const ModelUpdate& m, std::string& out) {
    Writer w(out, Type::ModelUpdate);
    w.u32(static_cast<uint32_t>(m.kind));
    w.u64(m.revision);
    w.u64(m.undo_floor);
    w.u64(m.next_id);
    w.str(m.bytes);
}

void encode(const PluginParameterChange& m, std::string& out) {
    Writer w(out, Type::PluginParameterChange);
    w.u64(m.seq);
    w.u64(m.insert);
    w.str(m.key);
    w.f32(m.value);
}

bool encode(const PluginCatalog& m, std::string& out, std::string& error) {
    if (m.plugins.size() > kMaxCatalogPlugins) {
        error = "a catalog of " + std::to_string(m.plugins.size()) + " plugins, over the cap of " +
                std::to_string(kMaxCatalogPlugins);
        return false;
    }
    for (const PluginInfo& p : m.plugins) {
        if (p.params.size() > kMaxPluginParams) {
            error = p.name + " has " + std::to_string(p.params.size()) +
                    " parameters, over the cap of " + std::to_string(kMaxPluginParams);
            return false;
        }
        if (p.categories.size() > kMaxPluginCategories) {
            error = p.name + " has " + std::to_string(p.categories.size()) +
                    " categories, over the cap of " + std::to_string(kMaxPluginCategories);
            return false;
        }
        for (const PluginParam& q : p.params) {
            if (q.key.size() > kMaxParamKeyBytes) {
                error = p.name + " parameter " + q.name + " has a key of " +
                        std::to_string(q.key.size()) + " bytes, over the cap of " +
                        std::to_string(kMaxParamKeyBytes);
                return false;
            }
            if (q.labels.size() > kMaxParamLabels) {
                error = p.name + " parameter " + q.name + " has " +
                        std::to_string(q.labels.size()) + " labels, over the cap of " +
                        std::to_string(kMaxParamLabels);
                return false;
            }
        }
    }
    Writer w(out, Type::PluginCatalog);
    w.u64(m.generation);
    w.u32(static_cast<uint32_t>(m.plugins.size()));
    for (const PluginInfo& p : m.plugins) {
        w.u32(static_cast<uint32_t>(p.format));
        w.str(p.id);
        w.str(p.name);
        w.str(p.vendor);
        w.str(p.version);
        w.list(p.categories);
        w.u32(p.inputs);
        w.u32(p.outputs);
        w.u32(p.usable ? 1 : 0);
        w.str(p.refusal);
        w.u32(p.has_editor ? 1 : 0);
        w.u32(static_cast<uint32_t>(p.params.size()));
        for (const PluginParam& q : p.params) {
            w.u32(q.index);
            w.str(q.key);
            w.str(q.name);
            w.f32(q.min);
            w.f32(q.max);
            w.f32(q.default_value);
            w.u32(q.flags);
            w.str(q.unit);
            w.u32(q.steps);
            w.list(q.labels);
        }
    }
    return true;
}

bool decode_hello_versions(std::string_view bytes, uint32_t& protocol_version,
                           uint32_t& state_layout_version, std::string& error) {
    Reader r(bytes, error);
    return r.type(Type::Hello) && r.u32(protocol_version) && r.u32(state_layout_version);
}

bool decode(std::string_view bytes, Hello& out, std::string& error) {
    Reader r(bytes, error);
    Hello m;
    if (!r.type(Type::Hello) || !r.u32(m.protocol_version) ||
        !r.u32(m.state_layout_version) || !r.u32(m.sample_rate) ||
        !r.u32(m.buffer_frames) || !r.u32(m.track_count) || !r.list(m.track_names) ||
        !r.list(m.implemented_commands) || !r.done()) {
        return false;
    }
    if (m.track_names.size() != m.track_count) {
        return r.fail("hello names " + std::to_string(m.track_names.size()) +
                      " tracks but counts " + std::to_string(m.track_count));
    }
    out = std::move(m);
    return true;
}

bool decode(std::string_view bytes, CommandRequest& out, std::string& error) {
    Reader r(bytes, error);
    CommandRequest m;
    if (!r.type(Type::CommandRequest) || !r.u64(m.seq) || !r.str(m.command) ||
        !r.i32(m.target) || !r.str(m.argument) || !r.done()) {
        return false;
    }
    out = std::move(m);
    return true;
}

bool decode(std::string_view bytes, ParameterChange& out, std::string& error) {
    Reader r(bytes, error);
    ParameterChange m;
    if (!r.type(Type::ParameterChange) || !r.u64(m.seq) || !r.u32(m.track) ||
        !r.enumeration(m.param, kLastParam) || !r.f32(m.value) || !r.done()) {
        return false;
    }
    out = m;
    return true;
}

bool decode(std::string_view bytes, ModelRequest& out, std::string& error) {
    Reader r(bytes, error);
    ModelRequest m;
    if (!r.type(Type::ModelRequest) || !r.u64(m.seq) ||
        !r.enumeration(m.action, kLastAction) || !r.str(m.op, kMaxModelBytes) ||
        !r.done()) {
        return false;
    }
    out = std::move(m);
    return true;
}

bool decode(std::string_view bytes, CommandReply& out, std::string& error) {
    Reader r(bytes, error);
    CommandReply m;
    if (!r.type(Type::CommandReply) || !r.u64(m.seq) ||
        !r.enumeration(m.result, kLastDispatchResult) || !r.str(m.message) || !r.done()) {
        return false;
    }
    out = std::move(m);
    return true;
}

bool decode(std::string_view bytes, EngineEvent& out, std::string& error) {
    Reader r(bytes, error);
    EngineEvent m;
    if (!r.type(Type::EngineEvent) || !r.enumeration(m.kind, kLastEvent) ||
        !r.i32(m.track) || !r.u64(m.frame) || !r.u64(m.insert) || !r.str(m.text) ||
        !r.done()) {
        return false;
    }
    out = std::move(m);
    return true;
}

bool decode(std::string_view bytes, ModelUpdate& out, std::string& error) {
    Reader r(bytes, error);
    ModelUpdate m;
    if (!r.type(Type::ModelUpdate) || !r.enumeration(m.kind, kLastUpdate) ||
        !r.u64(m.revision) || !r.u64(m.undo_floor) || !r.u64(m.next_id) ||
        !r.str(m.bytes, kMaxModelBytes) || !r.done()) {
        return false;
    }
    out = std::move(m);
    return true;
}

bool decode(std::string_view bytes, PluginParameterChange& out, std::string& error) {
    Reader r(bytes, error);
    PluginParameterChange m;
    if (!r.type(Type::PluginParameterChange) || !r.u64(m.seq) || !r.u64(m.insert) ||
        !r.str(m.key, kMaxParamKeyBytes) || !r.f32(m.value) || !r.done()) {
        return false;
    }
    out = std::move(m);
    return true;
}

bool decode(std::string_view bytes, PluginCatalog& out, std::string& error) {
    Reader r(bytes, error);
    PluginCatalog m;
    uint32_t plugins = 0;
    if (!r.type(Type::PluginCatalog) || !r.u64(m.generation) ||
        !r.count(plugins, kMaxCatalogPlugins, "plugins")) {
        return false;
    }
    for (uint32_t i = 0; i < plugins; ++i) {
        PluginInfo p;
        uint32_t params = 0;
        if (!r.enumeration(p.format, kLastFormat) || !r.str(p.id) || !r.str(p.name) ||
            !r.str(p.vendor) || !r.str(p.version) ||
            !r.list(p.categories, kMaxPluginCategories) || !r.u32(p.inputs) || !r.u32(p.outputs) ||
            !r.flag(p.usable) || !r.str(p.refusal) || !r.flag(p.has_editor) ||
            !r.count(params, kMaxPluginParams, "parameters")) {
            return false;
        }
        for (uint32_t k = 0; k < params; ++k) {
            PluginParam q;
            if (!r.u32(q.index) || !r.str(q.key, kMaxParamKeyBytes) || !r.str(q.name) ||
                !r.f32(q.min) || !r.f32(q.max) || !r.f32(q.default_value) || !r.u32(q.flags) ||
                !r.str(q.unit) || !r.u32(q.steps) || !r.list(q.labels, kMaxParamLabels)) {
                return false;
            }
            const std::string where = p.name + " parameter " + std::to_string(k);
            if (q.index != k) {
                return r.fail(where + " has dense index " + std::to_string(q.index));
            }
            // Written so that NaN fails it.
            if (!(std::isfinite(q.min) && std::isfinite(q.max) && q.min <= q.max &&
                  q.default_value >= q.min && q.default_value <= q.max)) {
                return r.fail(where + " has a range that is not finite, inverted, or does not"
                                      " hold its default");
            }
            if ((q.flags & ~kKnownParamFlags) != 0) {
                return r.fail(where + " has flags this version does not define");
            }
            if (q.steps == 1) {
                return r.fail(where + " has 1 step; a stepped parameter takes at least 2");
            }
            if (!q.labels.empty() && q.labels.size() != q.steps) {
                return r.fail(where + " has " + std::to_string(q.labels.size()) +
                              " labels for " + std::to_string(q.steps) + " steps");
            }
            p.params.push_back(std::move(q));
        }
        m.plugins.push_back(std::move(p));
    }
    if (!r.done()) return false;
    out = std::move(m);
    return true;
}

bool decode_into(std::string_view bytes, Inbox& inbox, std::string& error) {
    Type type{};
    if (!peek_type(bytes, type)) {
        error = "an empty message";
        return false;
    }
    switch (type) {
        case Type::CommandReply: {
            CommandReply m;
            if (!decode(bytes, m, error)) return false;
            inbox.replies.push_back(std::move(m));
            return true;
        }
        case Type::EngineEvent: {
            EngineEvent m;
            if (!decode(bytes, m, error)) return false;
            inbox.events.push_back(std::move(m));
            return true;
        }
        case Type::ModelUpdate: {
            ModelUpdate m;
            if (!decode(bytes, m, error)) return false;
            inbox.model.push_back(std::move(m));
            return true;
        }
        case Type::PluginCatalog: {
            PluginCatalog m;
            if (!decode(bytes, m, error)) return false;
            inbox.catalogs.push_back(std::move(m));
            return true;
        }
        case Type::Hello:
        case Type::CommandRequest:
        case Type::ParameterChange:
        case Type::ModelRequest:
        case Type::PluginParameterChange:
            break;
    }
    error = "message type " + std::to_string(static_cast<unsigned>(type)) +
            " is not one an engine sends to a client";
    return false;
}

std::string version_mismatch(uint32_t protocol_version, uint32_t state_layout_version) {
    std::string why;
    if (protocol_version != kProtocolVersion) {
        why = "the engine speaks control protocol version " +
              std::to_string(protocol_version) + " and this interface speaks version " +
              std::to_string(kProtocolVersion);
    }
    if (state_layout_version != kStateLayoutVersion) {
        if (!why.empty()) why += ", and ";
        why += "the engine's state block is layout version " +
               std::to_string(state_layout_version) + " where this interface reads version " +
               std::to_string(kStateLayoutVersion);
    }
    if (!why.empty()) {
        why = "refusing to connect: " + why +
              "; install an interface and an engine from the same build";
    }
    return why;
}

namespace {

void put_u32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

uint32_t get_u32(std::string_view in, std::size_t at) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<uint32_t>(static_cast<uint8_t>(in[at + i])) << (8 * i);
    }
    return v;
}

}  // namespace

bool split_frames(std::string_view message, std::vector<std::string>& frames,
                  std::string& error) {
    if (message.empty()) {
        error = "an empty message has nothing to send";
        return false;
    }
    if (message.size() > kMaxMessageBytes) {
        error = "a message of " + std::to_string(message.size()) + " bytes, over the cap of " +
                std::to_string(kMaxMessageBytes);
        return false;
    }
    constexpr std::size_t kRoom = kMaxFrameBytes - kFrameHeaderBytes;
    for (std::size_t at = 0; at < message.size(); at += kRoom) {
        const std::size_t n = std::min(kRoom, message.size() - at);
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

Reassembler::Result Reassembler::add(std::string_view frame, std::string& message,
                                     std::string& error) {
    if (frame.size() > kMaxFrameBytes) {
        return fail(error, "a frame of " + std::to_string(frame.size()) + " bytes, over the " +
                               std::to_string(kMaxFrameBytes) + " a frame may be");
    }
    if (frame.size() < kFrameHeaderBytes) {
        return fail(error, "a frame shorter than its " + std::to_string(kFrameHeaderBytes) +
                               "-byte header");
    }
    const std::size_t total = get_u32(frame, 0);
    const std::size_t offset = get_u32(frame, 4);
    const std::size_t n = frame.size() - kFrameHeaderBytes;
    if (total == 0) return fail(error, "a frame of an empty message");
    if (total > kMaxMessageBytes) {
        return fail(error, "a message of " + std::to_string(total) + " bytes, over the cap of " +
                               std::to_string(kMaxMessageBytes));
    }
    if (n == 0) return fail(error, "a frame that carries nothing");
    if (offset != partial_.size()) {
        return fail(error, "a frame at offset " + std::to_string(offset) + " where offset " +
                               std::to_string(partial_.size()) + " was expected");
    }
    if (expected_ != 0 && total != expected_) {
        return fail(error, "a frame of a " + std::to_string(total) +
                               " bytes message in the middle of a " +
                               std::to_string(expected_) + " bytes one");
    }
    if (offset + n > total) {
        return fail(error, "a frame that runs past the end of its " + std::to_string(total) +
                               "-byte message");
    }
    expected_ = total;
    partial_.append(frame.data() + kFrameHeaderBytes, n);
    if (partial_.size() < expected_) return Result::Partial;
    message.swap(partial_);
    partial_.clear();
    expected_ = 0;
    return Result::Complete;
}

}  // namespace sonora::api::wire
