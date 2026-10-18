// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/model/op_codec.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

#include "sonora/json/json.h"

namespace sonora::model {

namespace {

using json::Value;

template <typename E>
struct Name {
    const char* text;
    E value;
};

// The session file's spellings (session_file.cpp), so an operation and a
// file say the same thing the same way.
constexpr Name<FadeShape> kShapes[] = {{"linear", FadeShape::Linear},
                                       {"equal_power", FadeShape::EqualPower},
                                       {"s_curve", FadeShape::SCurve}};
constexpr Name<Align> kAligns[] = {
    {"centre", Align::Centre}, {"pre", Align::Pre}, {"post", Align::Post}};
constexpr Name<AudioFormat> kFormats[] = {{"int24", AudioFormat::Int24},
                                          {"float32", AudioFormat::Float32}};
constexpr Name<SourceKind> kKinds[] = {{"recorded", SourceKind::Recorded},
                                       {"imported", SourceKind::Imported}};
constexpr Name<MonitorMode> kMonitors[] = {{"auto", MonitorMode::Auto},
                                           {"input", MonitorMode::Input},
                                           {"off", MonitorMode::Off}};
constexpr Name<Fps> kFps[] = {{"23.976", Fps::F23_976}, {"24", Fps::F24},
                              {"25", Fps::F25},         {"29.97", Fps::F29_97},
                              {"29.97df", Fps::F29_97df}, {"30", Fps::F30}};
constexpr Name<PluginFormat> kPluginFormats[] = {{"lv2", PluginFormat::Lv2},
                                                 {"vst3", PluginFormat::Vst3}};
constexpr Name<Edge> kEdges[] = {{"start", Edge::Start}, {"end", Edge::End}};
constexpr Name<FadeEnd> kFadeEnds[] = {{"in", FadeEnd::In}, {"out", FadeEnd::Out}};

template <typename E, std::size_t N>
Value name_of(const Name<E> (&table)[N], E v) {
    for (const auto& n : table) {
        if (n.value == v) return Value::string(n.text);
    }
    return Value::string(table[0].text);
}

// --- writing ----------------------------------------------------------------

Value integer(std::int64_t n) { return Value::integer(n); }
Value id_value(Id id) { return Value::integer(static_cast<std::int64_t>(id)); }

Value pair(std::int64_t a, std::int64_t b) {
    Value v = Value::array();
    v.push(integer(a));
    v.push(integer(b));
    return v;
}

Value fade_value(const Fade& f) {
    Value v = Value::object();
    v.set("length", integer(f.length));
    v.set("shape", name_of(kShapes, f.shape));
    return v;
}

Value region_value(const Region& r) {
    Value v = Value::object();
    v.set("id", id_value(r.id));
    v.set("name", Value::string(r.name));
    v.set("position", integer(r.position));
    v.set("length", integer(r.length));
    Value& src = v.set("source", Value::array());
    for (const SourceRef& c : r.channels) {
        Value& e = src.push(Value::object());
        e.set("id", id_value(c.source));
        e.set("channel", integer(c.channel));
    }
    v.set("source_offset", integer(r.source_offset));
    v.set("gain_db", Value::number_float(r.gain_db));
    v.set("mute", Value::boolean(r.mute));
    v.set("fade_in", fade_value(r.fade_in));
    v.set("fade_out", fade_value(r.fade_out));
    return v;
}

Value crossfade_value(const Crossfade& x) {
    Value v = Value::object();
    v.set("left", id_value(x.left));
    v.set("right", id_value(x.right));
    v.set("length", integer(x.length));
    v.set("align", name_of(kAligns, x.align));
    v.set("shape", name_of(kShapes, x.shape));
    return v;
}

Value source_value(const Source& s) {
    Value v = Value::object();
    v.set("id", id_value(s.id));
    v.set("path", Value::string(s.path));
    v.set("channels", integer(s.channels));
    v.set("length", integer(s.length));
    v.set("format", name_of(kFormats, s.format));
    v.set("time_reference", integer(s.time_reference));
    v.set("umid", Value::string(s.umid));
    v.set("kind", name_of(kKinds, s.kind));
    v.set("take", integer(s.take));
    v.set("recorded_on_track", id_value(s.recorded_on_track));
    v.set("latency_compensation", integer(s.latency_compensation));
    return v;
}

Value mix_value(const TrackMix& m) {
    Value v = Value::object();
    v.set("track", id_value(m.track));
    Value& input = v.set("input", Value::array());
    for (const std::uint16_t c : m.input) input.push(integer(c));
    v.set("monitor", name_of(kMonitors, m.monitor));
    v.set("gain_db", Value::number_float(m.gain_db));
    v.set("pan", Value::number_float(m.pan));
    v.set("mute", Value::boolean(m.mute));
    v.set("solo", Value::boolean(m.solo));
    return v;
}

Value bus_value(const MonitorBus& b) {
    Value v = Value::object();
    v.set("outputs", pair(b.outputs[0], b.outputs[1]));
    v.set("gain_db", Value::number_float(b.gain_db));
    v.set("pan_law_db", Value::number_float(b.pan_law_db));
    return v;
}

Value settings_value(const SessionSettings& s) {
    Value v = Value::object();
    v.set("name", Value::string(s.name));
    v.set("sample_rate", integer(s.sample_rate));
    v.set("audio_format", name_of(kFormats, s.audio_format));
    Value& tc = v.set("timecode", Value::object());
    tc.set("origin", integer(s.timecode.origin));
    tc.set("fps", name_of(kFps, s.timecode.fps));
    Value& tempo = v.set("tempo", Value::array());
    for (const TempoEntry& t : s.tempo) {
        Value& e = tempo.push(Value::object());
        e.set("at", integer(t.at));
        e.set("bpm", Value::number_float(t.bpm));
        e.set("meter", pair(t.meter[0], t.meter[1]));
    }
    Value& tr = v.set("transport", Value::object());
    tr.set("loop", pair(s.transport.loop[0], s.transport.loop[1]));
    tr.set("punch", pair(s.transport.punch[0], s.transport.punch[1]));
    tr.set("pre_roll", integer(s.transport.pre_roll));
    tr.set("post_roll", integer(s.transport.post_roll));
    tr.set("punch_crossfade", integer(s.transport.punch_crossfade));
    return v;
}

Value marker_value(const Marker& m) {
    Value v = Value::object();
    v.set("id", id_value(m.id));
    v.set("position", integer(m.position));
    v.set("name", Value::string(m.name));
    v.set("length", integer(m.length));
    return v;
}

Value plugin_value(const PluginRef& p) {
    Value v = Value::object();
    v.set("format", name_of(kPluginFormats, p.format));
    v.set("id", Value::string(p.id));
    v.set("name", Value::string(p.name));
    v.set("version", Value::string(p.version));
    return v;
}

Value params_value(const std::vector<ParamValue>& params) {
    Value v = Value::array();
    for (const ParamValue& p : params) {
        Value& e = v.push(Value::object());
        e.set("key", Value::string(p.key));
        e.set("value", Value::number_float(p.value));
    }
    return v;
}

// Every key is always present in an operation, so "no state" is null.
Value state_value(const std::optional<StateRef>& s) {
    if (!s) return Value::null();
    Value v = Value::object();
    v.set("file", Value::string(s->file));
    v.set("sha256", Value::string(s->sha256));
    v.set("bytes", integer(static_cast<std::int64_t>(s->bytes)));
    return v;
}

Value insert_value(const Insert& in) {
    Value v = Value::object();
    v.set("id", id_value(in.id));
    v.set("slot", integer(in.slot));
    v.set("plugin", plugin_value(in.plugin));
    v.set("active", Value::boolean(in.active));
    return v;
}

Value insert_mix_value(const InsertMix& m) {
    Value v = Value::object();
    v.set("insert", id_value(m.insert));
    v.set("bypass", Value::boolean(m.bypass));
    v.set("params", params_value(m.params));
    v.set("state", state_value(m.state));
    return v;
}

Value with_kind(const char* kind) {
    Value v = Value::object();
    v.set("op", Value::string(kind));
    return v;
}

struct Encoder {
    Value operator()(const AddSource& o) const {
        Value v = with_kind("AddSource");
        v.set("source", source_value(o.source));
        return v;
    }
    Value operator()(const SetMix& o) const {
        Value v = with_kind("SetMix");
        v.set("mix", mix_value(o.mix));
        return v;
    }
    Value operator()(const SetMonitorBus& o) const {
        Value v = with_kind("SetMonitorBus");
        v.set("bus", bus_value(o.bus));
        return v;
    }
    Value operator()(const SetSettings& o) const {
        Value v = with_kind("SetSettings");
        v.set("settings", settings_value(o.settings));
        return v;
    }
    Value operator()(const AddTrack& o) const {
        Value v = with_kind("AddTrack");
        v.set("track", id_value(o.track));
        v.set("name", Value::string(o.name));
        v.set("channels", integer(o.channels));
        v.set("playlist", id_value(o.playlist));
        v.set("playlist_name", Value::string(o.playlist_name));
        v.set("mix", mix_value(o.mix));
        return v;
    }
    Value operator()(const PlaceRegion& o) const {
        Value v = with_kind("PlaceRegion");
        v.set("playlist", id_value(o.playlist));
        v.set("region", region_value(o.region));
        v.set("split_right_id", id_value(o.split_right_id));
        return v;
    }
    Value operator()(const RecordTake& o) const {
        Value v = with_kind("RecordTake");
        v.set("track", id_value(o.track));
        v.set("playlist", id_value(o.playlist));
        v.set("region", region_value(o.region));
        v.set("split_right_id", id_value(o.split_right_id));
        Value& x = v.set("crossfades", Value::array());
        for (const Crossfade& c : o.crossfades) x.push(crossfade_value(c));
        return v;
    }
    Value operator()(const MoveRegion& o) const {
        Value v = with_kind("MoveRegion");
        v.set("playlist", id_value(o.playlist));
        v.set("region", id_value(o.region));
        v.set("position", integer(o.position));
        v.set("split_right_id", id_value(o.split_right_id));
        return v;
    }
    Value operator()(const TrimRegion& o) const {
        Value v = with_kind("TrimRegion");
        v.set("playlist", id_value(o.playlist));
        v.set("region", id_value(o.region));
        v.set("edge", name_of(kEdges, o.edge));
        v.set("to", integer(o.to));
        return v;
    }
    Value operator()(const SplitRegion& o) const {
        Value v = with_kind("SplitRegion");
        v.set("playlist", id_value(o.playlist));
        v.set("region", id_value(o.region));
        v.set("at", integer(o.at));
        v.set("new_right_id", id_value(o.new_right_id));
        return v;
    }
    Value operator()(const RemoveRegion& o) const {
        Value v = with_kind("RemoveRegion");
        v.set("playlist", id_value(o.playlist));
        v.set("region", id_value(o.region));
        return v;
    }
    Value operator()(const SetFade& o) const {
        Value v = with_kind("SetFade");
        v.set("playlist", id_value(o.playlist));
        v.set("region", id_value(o.region));
        v.set("end", name_of(kFadeEnds, o.end));
        v.set("fade", fade_value(o.fade));
        return v;
    }
    Value operator()(const SetCrossfade& o) const {
        Value v = with_kind("SetCrossfade");
        v.set("playlist", id_value(o.playlist));
        v.set("crossfade", crossfade_value(o.crossfade));
        return v;
    }
    Value operator()(const RemoveCrossfade& o) const {
        Value v = with_kind("RemoveCrossfade");
        v.set("playlist", id_value(o.playlist));
        v.set("left", id_value(o.left));
        v.set("right", id_value(o.right));
        return v;
    }
    Value operator()(const AddPlaylist& o) const {
        Value v = with_kind("AddPlaylist");
        v.set("track", id_value(o.track));
        v.set("playlist", id_value(o.playlist));
        v.set("name", Value::string(o.name));
        return v;
    }
    Value operator()(const SetActivePlaylist& o) const {
        Value v = with_kind("SetActivePlaylist");
        v.set("track", id_value(o.track));
        v.set("playlist", id_value(o.playlist));
        return v;
    }
    Value operator()(const AddMarker& o) const {
        Value v = with_kind("AddMarker");
        v.set("marker", marker_value(o.marker));
        return v;
    }
    Value operator()(const MoveMarker& o) const {
        Value v = with_kind("MoveMarker");
        v.set("marker", id_value(o.marker));
        v.set("position", integer(o.position));
        return v;
    }
    Value operator()(const RemoveMarker& o) const {
        Value v = with_kind("RemoveMarker");
        v.set("marker", id_value(o.marker));
        return v;
    }
    Value operator()(const SetTrackName& o) const {
        Value v = with_kind("SetTrackName");
        v.set("track", id_value(o.track));
        v.set("name", Value::string(o.name));
        return v;
    }
    Value operator()(const ReorderTracks& o) const {
        Value v = with_kind("ReorderTracks");
        Value& order = v.set("order", Value::array());
        for (const Id id : o.order) order.push(id_value(id));
        return v;
    }
    Value operator()(const AddInsert& o) const {
        Value v = with_kind("AddInsert");
        v.set("track", id_value(o.track));
        v.set("insert", id_value(o.insert));
        v.set("slot", integer(o.slot));
        v.set("plugin", plugin_value(o.plugin));
        v.set("params", params_value(o.params));
        v.set("state", state_value(o.state));
        return v;
    }
    Value operator()(const RemoveInsert& o) const {
        Value v = with_kind("RemoveInsert");
        v.set("insert", id_value(o.insert));
        return v;
    }
    Value operator()(const MoveInsert& o) const {
        Value v = with_kind("MoveInsert");
        v.set("insert", id_value(o.insert));
        v.set("to_track", id_value(o.to_track));
        v.set("to_slot", integer(o.to_slot));
        return v;
    }
    Value operator()(const SetInsertActive& o) const {
        Value v = with_kind("SetInsertActive");
        v.set("insert", id_value(o.insert));
        v.set("active", Value::boolean(o.active));
        return v;
    }
    Value operator()(const ReplaceInsert& o) const {
        Value v = with_kind("ReplaceInsert");
        v.set("insert", id_value(o.insert));
        v.set("replacement", id_value(o.replacement));
        v.set("plugin", plugin_value(o.plugin));
        v.set("params", params_value(o.params));
        v.set("state", state_value(o.state));
        return v;
    }
    Value operator()(const SetInsertBypass& o) const {
        Value v = with_kind("SetInsertBypass");
        v.set("insert", id_value(o.insert));
        v.set("bypass", Value::boolean(o.bypass));
        return v;
    }
    Value operator()(const SetInsertParams& o) const {
        Value v = with_kind("SetInsertParams");
        v.set("insert", id_value(o.insert));
        v.set("params", params_value(o.params));
        return v;
    }
    Value operator()(const SetInsertState& o) const {
        Value v = with_kind("SetInsertState");
        v.set("insert", id_value(o.insert));
        v.set("state", state_value(o.state));
        return v;
    }
    // Each member exactly as it would be alone; no label key when there is
    // none, so the encoding stays canonical.
    Value operator()(const Compound& o) const {
        Value v = with_kind("Compound");
        Value ops = Value::array();
        for (const Member& m : o.ops) ops.push(std::visit(*this, m));
        v.set("ops", std::move(ops));
        if (!o.label.empty()) v.set("label", Value::string(o.label));
        return v;
    }
};

// --- reading ----------------------------------------------------------------

class Reader {
public:
    // `prefix` is where the value read sits in a larger one ("ops[3]" for a
    // compound's member), so its errors name the whole path.
    explicit Reader(std::string& error, std::string prefix = {})
        : error_(error), prefix_(std::move(prefix)) {}

    bool fail(const std::string& path, const std::string& msg) {
        const std::string full =
            prefix_.empty() ? path : (path.empty() ? prefix_ : prefix_ + "." + path);
        error_ = full.empty() ? msg : full + ": " + msg;
        return false;
    }

    bool keys(const Value& v, const std::string& path,
              std::initializer_list<const char*> required) {
        if (!v.is_object()) return fail(path, "must be an object");
        for (const auto& kv : v.members()) {
            bool known = false;
            for (const char* k : required) known = known || kv.first == k;
            if (!known) return fail(path, "unknown key \"" + kv.first + "\"");
        }
        for (const char* k : required) {
            if (v.find(k) == nullptr) {
                return fail(path, std::string("missing required key \"") + k + "\"");
            }
        }
        return true;
    }

    bool integer(const Value& obj, const std::string& path, const char* key,
                 std::int64_t max, std::int64_t& out) {
        const Value& v = *obj.find(key);
        std::int64_t n = 0;
        if (!v.is_number() || !v.is_integer_lexeme() || !v.to_int64(n) || n < 0 || n > max) {
            return fail(at(path, key), "must be an integer from 0 to " + std::to_string(max));
        }
        out = n;
        return true;
    }

    template <typename T>
    bool small(const Value& obj, const std::string& path, const char* key, T& out) {
        std::int64_t n = 0;
        constexpr auto max = static_cast<std::int64_t>(std::numeric_limits<T>::max());
        if (!integer(obj, path, key, std::min(max, kMaxInteger), n)) return false;
        out = static_cast<T>(n);
        return true;
    }

    bool id(const Value& obj, const std::string& path, const char* key, Id& out) {
        std::int64_t n = 0;
        if (!integer(obj, path, key, kMaxInteger, n)) return false;
        out = static_cast<Id>(n);
        return true;
    }

    bool frames(const Value& obj, const std::string& path, const char* key, Frames& out) {
        return integer(obj, path, key, kMaxInteger, out);
    }

    bool real(const Value& obj, const std::string& path, const char* key, float& out) {
        const Value& v = *obj.find(key);
        if (!v.is_number() || !v.to_float(out)) return fail(at(path, key), "must be a number");
        return true;
    }

    bool text(const Value& obj, const std::string& path, const char* key, std::string& out) {
        const Value& v = *obj.find(key);
        if (!v.is_string()) return fail(at(path, key), "must be a string");
        out = v.text();
        return true;
    }

    bool flag(const Value& obj, const std::string& path, const char* key, bool& out) {
        const Value& v = *obj.find(key);
        if (!v.is_bool()) return fail(at(path, key), "must be true or false");
        out = v.as_bool();
        return true;
    }

    template <typename E, std::size_t N>
    bool choice(const Value& obj, const std::string& path, const char* key,
                const Name<E> (&table)[N], E& out) {
        const Value& v = *obj.find(key);
        for (const auto& n : table) {
            if (v.is_string() && v.text() == n.text) {
                out = n.value;
                return true;
            }
        }
        return fail(at(path, key), "is not a value this version knows");
    }

    bool array(const Value& obj, const std::string& path, const char* key, const Value*& out) {
        const Value& v = *obj.find(key);
        if (!v.is_array()) return fail(at(path, key), "must be an array");
        out = &v;
        return true;
    }

    bool range(const Value& obj, const std::string& path, const char* key, std::int64_t max,
               std::array<std::int64_t, 2>& out) {
        const Value& v = *obj.find(key);
        if (!v.is_array() || v.items().size() != 2) return fail(at(path, key), "must be a pair");
        for (std::size_t i = 0; i < 2; ++i) {
            const Value& e = v.items()[i];
            std::int64_t n = 0;
            if (!e.is_integer_lexeme() || !e.to_int64(n) || n < 0 || n > max) {
                return fail(at(path, key), "must be a pair of integers from 0 to " +
                                               std::to_string(max));
            }
            out[i] = n;
        }
        return true;
    }

    static std::string at(const std::string& path, const char* key) {
        return path.empty() ? std::string(key) : path + "." + key;
    }

private:
    std::string& error_;
    std::string prefix_;
};

bool read_fade(Reader& rd, const Value& obj, const std::string& path, const char* key, Fade& f) {
    const Value& v = *obj.find(key);
    const std::string p = Reader::at(path, key);
    return rd.keys(v, p, {"length", "shape"}) && rd.frames(v, p, "length", f.length) &&
           rd.choice(v, p, "shape", kShapes, f.shape);
}

bool read_region_value(Reader& rd, const Value& v, const std::string& p, Region& r) {
    if (!rd.keys(v, p, {"id", "name", "position", "length", "source", "source_offset",
                        "gain_db", "mute", "fade_in", "fade_out"})) {
        return false;
    }
    if (!rd.id(v, p, "id", r.id) || !rd.text(v, p, "name", r.name) ||
        !rd.frames(v, p, "position", r.position) || !rd.frames(v, p, "length", r.length)) {
        return false;
    }
    const Value* src = nullptr;
    if (!rd.array(v, p, "source", src)) return false;
    for (const Value& e : src->items()) {
        const std::string sp = p + ".source";
        if (!rd.keys(e, sp, {"id", "channel"})) return false;
        SourceRef ref;
        if (!rd.id(e, sp, "id", ref.source) || !rd.small(e, sp, "channel", ref.channel)) {
            return false;
        }
        r.channels.push_back(ref);
    }
    return rd.frames(v, p, "source_offset", r.source_offset) &&
           rd.real(v, p, "gain_db", r.gain_db) && rd.flag(v, p, "mute", r.mute) &&
           read_fade(rd, v, p, "fade_in", r.fade_in) &&
           read_fade(rd, v, p, "fade_out", r.fade_out);
}

bool read_region(Reader& rd, const Value& obj, const std::string& path, const char* key,
                 Region& r) {
    return read_region_value(rd, *obj.find(key), Reader::at(path, key), r);
}

bool read_crossfade(Reader& rd, const Value& v, const std::string& p, Crossfade& x) {
    return rd.keys(v, p, {"left", "right", "length", "align", "shape"}) &&
           rd.id(v, p, "left", x.left) && rd.id(v, p, "right", x.right) &&
           rd.frames(v, p, "length", x.length) && rd.choice(v, p, "align", kAligns, x.align) &&
           rd.choice(v, p, "shape", kShapes, x.shape);
}

bool read_source_value(Reader& rd, const Value& v, const std::string& p, Source& s) {
    return rd.keys(v, p, {"id", "path", "channels", "length", "format", "time_reference",
                          "umid", "kind", "take", "recorded_on_track",
                          "latency_compensation"}) &&
           rd.id(v, p, "id", s.id) && rd.text(v, p, "path", s.path) &&
           rd.small(v, p, "channels", s.channels) && rd.frames(v, p, "length", s.length) &&
           rd.choice(v, p, "format", kFormats, s.format) &&
           rd.integer(v, p, "time_reference", kMaxInteger, s.time_reference) &&
           rd.text(v, p, "umid", s.umid) && rd.choice(v, p, "kind", kKinds, s.kind) &&
           rd.integer(v, p, "take", kMaxInteger, s.take) &&
           rd.id(v, p, "recorded_on_track", s.recorded_on_track) &&
           rd.frames(v, p, "latency_compensation", s.latency_compensation);
}

bool read_source(Reader& rd, const Value& obj, const std::string& path, Source& s) {
    return read_source_value(rd, *obj.find("source"), Reader::at(path, "source"), s);
}

bool read_mix_value(Reader& rd, const Value& v, const std::string& p, TrackMix& m) {
    if (!rd.keys(v, p, {"track", "input", "monitor", "gain_db", "pan", "mute", "solo"}) ||
        !rd.id(v, p, "track", m.track)) {
        return false;
    }
    const Value* input = nullptr;
    if (!rd.array(v, p, "input", input)) return false;
    for (const Value& e : input->items()) {
        std::int64_t n = 0;
        if (!e.is_integer_lexeme() || !e.to_int64(n) || n < 0 || n > 0xFFFF) {
            return rd.fail(p + ".input", "must hold capture channel indices from 0 to 65535");
        }
        m.input.push_back(static_cast<std::uint16_t>(n));
    }
    return rd.choice(v, p, "monitor", kMonitors, m.monitor) &&
           rd.real(v, p, "gain_db", m.gain_db) && rd.real(v, p, "pan", m.pan) &&
           rd.flag(v, p, "mute", m.mute) && rd.flag(v, p, "solo", m.solo);
}

bool read_mix(Reader& rd, const Value& obj, const std::string& path, TrackMix& m) {
    return read_mix_value(rd, *obj.find("mix"), Reader::at(path, "mix"), m);
}

bool read_bus(Reader& rd, const Value& obj, MonitorBus& b) {
    const Value& v = *obj.find("bus");
    std::array<std::int64_t, 2> outs{};
    if (!rd.keys(v, "bus", {"outputs", "gain_db", "pan_law_db"}) ||
        !rd.range(v, "bus", "outputs", 0xFFFF, outs)) {
        return false;
    }
    b.outputs = {{static_cast<std::uint16_t>(outs[0]), static_cast<std::uint16_t>(outs[1])}};
    return rd.real(v, "bus", "gain_db", b.gain_db) &&
           rd.real(v, "bus", "pan_law_db", b.pan_law_db);
}

bool read_settings(Reader& rd, const Value& obj, SessionSettings& s) {
    const Value& v = *obj.find("settings");
    const std::string p = "settings";
    if (!rd.keys(v, p, {"name", "sample_rate", "audio_format", "timecode", "tempo",
                        "transport"})) {
        return false;
    }
    std::int64_t rate = 0;
    if (!rd.text(v, p, "name", s.name) || !rd.integer(v, p, "sample_rate", 0xFFFFFFFF, rate) ||
        !rd.choice(v, p, "audio_format", kFormats, s.audio_format)) {
        return false;
    }
    s.sample_rate = static_cast<std::uint32_t>(rate);
    const Value& tc = *v.find("timecode");
    if (!rd.keys(tc, p + ".timecode", {"origin", "fps"}) ||
        !rd.integer(tc, p + ".timecode", "origin", kMaxInteger, s.timecode.origin) ||
        !rd.choice(tc, p + ".timecode", "fps", kFps, s.timecode.fps)) {
        return false;
    }
    const Value* tempo = nullptr;
    if (!rd.array(v, p, "tempo", tempo)) return false;
    s.tempo.clear();
    for (const Value& e : tempo->items()) {
        const std::string tp = p + ".tempo";
        TempoEntry t;
        std::array<std::int64_t, 2> meter{};
        if (!rd.keys(e, tp, {"at", "bpm", "meter"}) || !rd.frames(e, tp, "at", t.at) ||
            !rd.real(e, tp, "bpm", t.bpm) || !rd.range(e, tp, "meter", 0xFFFF, meter)) {
            return false;
        }
        t.meter = {{static_cast<std::uint32_t>(meter[0]), static_cast<std::uint32_t>(meter[1])}};
        s.tempo.push_back(t);
    }
    const Value& tr = *v.find("transport");
    const std::string trp = p + ".transport";
    std::array<std::int64_t, 2> loop{}, punch{};
    if (!rd.keys(tr, trp, {"loop", "punch", "pre_roll", "post_roll", "punch_crossfade"}) ||
        !rd.range(tr, trp, "loop", kMaxInteger, loop) ||
        !rd.range(tr, trp, "punch", kMaxInteger, punch) ||
        !rd.frames(tr, trp, "pre_roll", s.transport.pre_roll) ||
        !rd.frames(tr, trp, "post_roll", s.transport.post_roll) ||
        !rd.frames(tr, trp, "punch_crossfade", s.transport.punch_crossfade)) {
        return false;
    }
    s.transport.loop = loop;
    s.transport.punch = punch;
    return true;
}

bool read_marker_value(Reader& rd, const Value& v, const std::string& p, Marker& m) {
    return rd.keys(v, p, {"id", "position", "name", "length"}) && rd.id(v, p, "id", m.id) &&
           rd.frames(v, p, "position", m.position) && rd.text(v, p, "name", m.name) &&
           rd.frames(v, p, "length", m.length);
}

bool read_marker(Reader& rd, const Value& obj, Marker& m) {
    return read_marker_value(rd, *obj.find("marker"), "marker", m);
}

bool read_plugin(Reader& rd, const Value& obj, const std::string& path, PluginRef& p) {
    const Value& v = *obj.find("plugin");
    const std::string pp = Reader::at(path, "plugin");
    return rd.keys(v, pp, {"format", "id", "name", "version"}) &&
           rd.choice(v, pp, "format", kPluginFormats, p.format) && rd.text(v, pp, "id", p.id) &&
           rd.text(v, pp, "name", p.name) && rd.text(v, pp, "version", p.version);
}

bool read_params(Reader& rd, const Value& obj, const std::string& path,
                 std::vector<ParamValue>& out) {
    const Value* list = nullptr;
    if (!rd.array(obj, path, "params", list)) return false;
    const std::string pp = Reader::at(path, "params");
    for (const Value& e : list->items()) {
        ParamValue p;
        if (!rd.keys(e, pp, {"key", "value"}) || !rd.text(e, pp, "key", p.key) ||
            !rd.real(e, pp, "value", p.value)) {
            return false;
        }
        out.push_back(std::move(p));
    }
    return true;
}

bool read_state(Reader& rd, const Value& obj, const std::string& path,
                std::optional<StateRef>& out) {
    const Value& v = *obj.find("state");
    if (v.is_null()) {
        out.reset();
        return true;
    }
    const std::string sp = Reader::at(path, "state");
    StateRef s;
    std::int64_t bytes = 0;
    if (!rd.keys(v, sp, {"file", "sha256", "bytes"}) || !rd.text(v, sp, "file", s.file) ||
        !rd.text(v, sp, "sha256", s.sha256) ||
        !rd.integer(v, sp, "bytes", kMaxInteger, bytes)) {
        return false;
    }
    s.bytes = static_cast<std::uint64_t>(bytes);
    out = std::move(s);
    return true;
}

bool read_insert_value(Reader& rd, const Value& v, const std::string& p, Insert& in) {
    return rd.keys(v, p, {"id", "slot", "plugin", "active"}) && rd.id(v, p, "id", in.id) &&
           rd.small(v, p, "slot", in.slot) && read_plugin(rd, v, p, in.plugin) &&
           rd.flag(v, p, "active", in.active);
}

bool read_insert_mix_value(Reader& rd, const Value& v, const std::string& p, InsertMix& m) {
    return rd.keys(v, p, {"insert", "bypass", "params", "state"}) &&
           rd.id(v, p, "insert", m.insert) && rd.flag(v, p, "bypass", m.bypass) &&
           read_params(rd, v, p, m.params) && read_state(rd, v, p, m.state);
}

bool decode_compound(Reader& rd, const Value& v, Operation& out);

bool decode_value(Reader& rd, const Value& v, const std::string& kind, Operation& out) {
    if (kind == "AddSource") {
        AddSource o;
        if (!rd.keys(v, "", {"op", "source"}) || !read_source(rd, v, "", o.source)) return false;
        out = std::move(o);
    } else if (kind == "SetMix") {
        SetMix o;
        if (!rd.keys(v, "", {"op", "mix"}) || !read_mix(rd, v, "", o.mix)) return false;
        out = std::move(o);
    } else if (kind == "SetMonitorBus") {
        SetMonitorBus o;
        if (!rd.keys(v, "", {"op", "bus"}) || !read_bus(rd, v, o.bus)) return false;
        out = o;
    } else if (kind == "SetSettings") {
        SetSettings o;
        if (!rd.keys(v, "", {"op", "settings"}) || !read_settings(rd, v, o.settings)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "AddTrack") {
        AddTrack o;
        if (!rd.keys(v, "", {"op", "track", "name", "channels", "playlist", "playlist_name",
                             "mix"}) ||
            !rd.id(v, "", "track", o.track) || !rd.text(v, "", "name", o.name) ||
            !rd.small(v, "", "channels", o.channels) || !rd.id(v, "", "playlist", o.playlist) ||
            !rd.text(v, "", "playlist_name", o.playlist_name) || !read_mix(rd, v, "", o.mix)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "PlaceRegion") {
        PlaceRegion o;
        if (!rd.keys(v, "", {"op", "playlist", "region", "split_right_id"}) ||
            !rd.id(v, "", "playlist", o.playlist) || !read_region(rd, v, "", "region", o.region) ||
            !rd.id(v, "", "split_right_id", o.split_right_id)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "RecordTake") {
        RecordTake o;
        const Value* xs = nullptr;
        if (!rd.keys(v, "", {"op", "track", "playlist", "region", "split_right_id",
                             "crossfades"}) ||
            !rd.id(v, "", "track", o.track) || !rd.id(v, "", "playlist", o.playlist) ||
            !read_region(rd, v, "", "region", o.region) ||
            !rd.id(v, "", "split_right_id", o.split_right_id) ||
            !rd.array(v, "", "crossfades", xs)) {
            return false;
        }
        for (const Value& e : xs->items()) {
            Crossfade x;
            if (!read_crossfade(rd, e, "crossfades", x)) return false;
            o.crossfades.push_back(x);
        }
        out = std::move(o);
    } else if (kind == "MoveRegion") {
        MoveRegion o;
        if (!rd.keys(v, "", {"op", "playlist", "region", "position", "split_right_id"}) ||
            !rd.id(v, "", "playlist", o.playlist) || !rd.id(v, "", "region", o.region) ||
            !rd.frames(v, "", "position", o.position) ||
            !rd.id(v, "", "split_right_id", o.split_right_id)) {
            return false;
        }
        out = o;
    } else if (kind == "TrimRegion") {
        TrimRegion o;
        if (!rd.keys(v, "", {"op", "playlist", "region", "edge", "to"}) ||
            !rd.id(v, "", "playlist", o.playlist) || !rd.id(v, "", "region", o.region) ||
            !rd.choice(v, "", "edge", kEdges, o.edge) || !rd.frames(v, "", "to", o.to)) {
            return false;
        }
        out = o;
    } else if (kind == "SplitRegion") {
        SplitRegion o;
        if (!rd.keys(v, "", {"op", "playlist", "region", "at", "new_right_id"}) ||
            !rd.id(v, "", "playlist", o.playlist) || !rd.id(v, "", "region", o.region) ||
            !rd.frames(v, "", "at", o.at) || !rd.id(v, "", "new_right_id", o.new_right_id)) {
            return false;
        }
        out = o;
    } else if (kind == "RemoveRegion") {
        RemoveRegion o;
        if (!rd.keys(v, "", {"op", "playlist", "region"}) ||
            !rd.id(v, "", "playlist", o.playlist) || !rd.id(v, "", "region", o.region)) {
            return false;
        }
        out = o;
    } else if (kind == "SetFade") {
        SetFade o;
        if (!rd.keys(v, "", {"op", "playlist", "region", "end", "fade"}) ||
            !rd.id(v, "", "playlist", o.playlist) || !rd.id(v, "", "region", o.region) ||
            !rd.choice(v, "", "end", kFadeEnds, o.end) || !read_fade(rd, v, "", "fade", o.fade)) {
            return false;
        }
        out = o;
    } else if (kind == "SetCrossfade") {
        SetCrossfade o;
        if (!rd.keys(v, "", {"op", "playlist", "crossfade"}) ||
            !rd.id(v, "", "playlist", o.playlist) ||
            !read_crossfade(rd, *v.find("crossfade"), "crossfade", o.crossfade)) {
            return false;
        }
        out = o;
    } else if (kind == "RemoveCrossfade") {
        RemoveCrossfade o;
        if (!rd.keys(v, "", {"op", "playlist", "left", "right"}) ||
            !rd.id(v, "", "playlist", o.playlist) || !rd.id(v, "", "left", o.left) ||
            !rd.id(v, "", "right", o.right)) {
            return false;
        }
        out = o;
    } else if (kind == "AddPlaylist") {
        AddPlaylist o;
        if (!rd.keys(v, "", {"op", "track", "playlist", "name"}) ||
            !rd.id(v, "", "track", o.track) || !rd.id(v, "", "playlist", o.playlist) ||
            !rd.text(v, "", "name", o.name)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "SetActivePlaylist") {
        SetActivePlaylist o;
        if (!rd.keys(v, "", {"op", "track", "playlist"}) || !rd.id(v, "", "track", o.track) ||
            !rd.id(v, "", "playlist", o.playlist)) {
            return false;
        }
        out = o;
    } else if (kind == "AddMarker") {
        AddMarker o;
        if (!rd.keys(v, "", {"op", "marker"}) || !read_marker(rd, v, o.marker)) return false;
        out = std::move(o);
    } else if (kind == "MoveMarker") {
        MoveMarker o;
        if (!rd.keys(v, "", {"op", "marker", "position"}) || !rd.id(v, "", "marker", o.marker) ||
            !rd.frames(v, "", "position", o.position)) {
            return false;
        }
        out = o;
    } else if (kind == "RemoveMarker") {
        RemoveMarker o;
        if (!rd.keys(v, "", {"op", "marker"}) || !rd.id(v, "", "marker", o.marker)) return false;
        out = o;
    } else if (kind == "SetTrackName") {
        SetTrackName o;
        if (!rd.keys(v, "", {"op", "track", "name"}) || !rd.id(v, "", "track", o.track) ||
            !rd.text(v, "", "name", o.name)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "ReorderTracks") {
        ReorderTracks o;
        const Value* order = nullptr;
        if (!rd.keys(v, "", {"op", "order"}) || !rd.array(v, "", "order", order)) return false;
        for (const Value& e : order->items()) {
            std::int64_t n = 0;
            if (!e.is_integer_lexeme() || !e.to_int64(n) || n < 0 || n > kMaxInteger) {
                return rd.fail("order", "must hold track ids");
            }
            o.order.push_back(static_cast<Id>(n));
        }
        out = std::move(o);
    } else if (kind == "AddInsert") {
        AddInsert o;
        if (!rd.keys(v, "", {"op", "track", "insert", "slot", "plugin", "params", "state"}) ||
            !rd.id(v, "", "track", o.track) || !rd.id(v, "", "insert", o.insert) ||
            !rd.small(v, "", "slot", o.slot) || !read_plugin(rd, v, "", o.plugin) ||
            !read_params(rd, v, "", o.params) || !read_state(rd, v, "", o.state)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "RemoveInsert") {
        RemoveInsert o;
        if (!rd.keys(v, "", {"op", "insert"}) || !rd.id(v, "", "insert", o.insert)) return false;
        out = o;
    } else if (kind == "MoveInsert") {
        MoveInsert o;
        if (!rd.keys(v, "", {"op", "insert", "to_track", "to_slot"}) ||
            !rd.id(v, "", "insert", o.insert) || !rd.id(v, "", "to_track", o.to_track) ||
            !rd.small(v, "", "to_slot", o.to_slot)) {
            return false;
        }
        out = o;
    } else if (kind == "SetInsertActive") {
        SetInsertActive o;
        if (!rd.keys(v, "", {"op", "insert", "active"}) || !rd.id(v, "", "insert", o.insert) ||
            !rd.flag(v, "", "active", o.active)) {
            return false;
        }
        out = o;
    } else if (kind == "ReplaceInsert") {
        ReplaceInsert o;
        if (!rd.keys(v, "", {"op", "insert", "replacement", "plugin", "params", "state"}) ||
            !rd.id(v, "", "insert", o.insert) || !rd.id(v, "", "replacement", o.replacement) ||
            !read_plugin(rd, v, "", o.plugin) || !read_params(rd, v, "", o.params) ||
            !read_state(rd, v, "", o.state)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "SetInsertBypass") {
        SetInsertBypass o;
        if (!rd.keys(v, "", {"op", "insert", "bypass"}) || !rd.id(v, "", "insert", o.insert) ||
            !rd.flag(v, "", "bypass", o.bypass)) {
            return false;
        }
        out = o;
    } else if (kind == "SetInsertParams") {
        SetInsertParams o;
        if (!rd.keys(v, "", {"op", "insert", "params"}) || !rd.id(v, "", "insert", o.insert) ||
            !read_params(rd, v, "", o.params)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "SetInsertState") {
        SetInsertState o;
        if (!rd.keys(v, "", {"op", "insert", "state"}) || !rd.id(v, "", "insert", o.insert) ||
            !read_state(rd, v, "", o.state)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "Compound") {
        return decode_compound(rd, v, out);
    } else {
        return rd.fail("op", "\"" + kind + "\" is not an operation this version knows");
    }
    return true;
}

// Members are refused by their JSON path, counted from 0 ("ops[3]: ..."),
// as the file and every other path in this codec count.
bool decode_compound(Reader& rd, const Value& v, Operation& out) {
    const bool labelled = v.is_object() && v.find("label") != nullptr;
    if (!(labelled ? rd.keys(v, "", {"op", "ops", "label"}) : rd.keys(v, "", {"op", "ops"}))) {
        return false;
    }
    const Value* ops = nullptr;
    if (!rd.array(v, "", "ops", ops)) return false;
    const std::size_t n = ops->items().size();
    if (n == 0) return rd.fail("ops", "a compound needs at least one operation; it is empty");
    if (n > kMaxCompoundMembers) {
        return rd.fail("ops", "at most " + std::to_string(kMaxCompoundMembers) +
                                  " operations in a compound; this one has " +
                                  std::to_string(n));
    }
    Compound c;
    c.ops.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Value& item = ops->items()[i];
        const std::string path = "ops[" + std::to_string(i) + "]";
        const Value* kind = item.is_object() ? item.find("op") : nullptr;
        if (kind == nullptr || !kind->is_string()) {
            return rd.fail(path, "must be an object whose \"op\" names the operation");
        }
        if (kind->text() == "Compound") return rd.fail(path, "a compound cannot hold a compound");
        std::string why;
        Reader member_reader(why, path);
        Operation op;
        if (!decode_value(member_reader, item, kind->text(), op)) return rd.fail("", why);
        c.ops.emplace_back();
        to_member(std::move(op), c.ops.back());
    }
    if (labelled) {
        if (!rd.text(v, "", "label", c.label)) return false;
        if (c.label.empty()) return rd.fail("label", "must not be empty; leave it out instead");
    }
    out = std::move(c);
    return true;
}

// --- the undo window ---------------------------------------------------------

// 2: tracks carry their inserts, and the mix the inserts' values.
// 3: each step carries its label (ADR-0009 update 2026-10-14), for "Undo
//    Paste" on a replica rebuilt from the window.
constexpr std::int64_t kWindowFormat = 3;

// Each table entry is written once and referred to by index, so a track or
// playlist that several steps share crosses once and is shared again on
// the replica, as it is on the engine.
template <typename T>
class Table {
public:
    // The index of `p`, and whether it is new to the table.
    std::size_t index(const T* p, bool& added) {
        const auto it = seen_.find(p);
        if (it != seen_.end()) {
            added = false;
            return it->second;
        }
        const std::size_t ix = seen_.size();
        seen_.emplace(p, ix);
        added = true;
        return ix;
    }

private:
    std::unordered_map<const T*, std::size_t> seen_;
};

Value index_value(std::size_t ix) { return Value::integer(static_cast<std::int64_t>(ix)); }

Value playlist_value(const Playlist& p) {
    Value v = Value::object();
    v.set("id", id_value(p.id));
    v.set("name", Value::string(p.name));
    Value& rs = v.set("regions", Value::array());
    for (const Region& r : p.regions) rs.push(region_value(r));
    Value& xs = v.set("crossfades", Value::array());
    for (const Crossfade& x : p.crossfades) xs.push(crossfade_value(x));
    return v;
}

class WindowWriter {
public:
    std::size_t edit(const EditState& e) {
        bool added = false;
        const std::size_t ix = edit_ix_.index(&e, added);
        if (!added) return ix;
        Value ev = Value::object();
        Value& ts = ev.set("tracks", Value::array());
        for (const auto& t : e.tracks) ts.push(index_value(track(*t)));
        if (e.markers) {
            ev.set("markers", index_value(markers(*e.markers)));
        } else {
            ev.set("markers", Value::null());
        }
        edits_.push(std::move(ev));
        return ix;
    }

    Value playlists_ = Value::array();
    Value tracks_ = Value::array();
    Value markers_ = Value::array();
    Value edits_ = Value::array();

private:
    std::size_t track(const TrackEdit& t) {
        bool added = false;
        const std::size_t ix = track_ix_.index(&t, added);
        if (!added) return ix;
        Value tv = Value::object();
        tv.set("id", id_value(t.id));
        tv.set("name", Value::string(t.name));
        tv.set("channels", integer(t.channels));
        tv.set("active_playlist", id_value(t.active_playlist));
        Value& ins = tv.set("inserts", Value::array());
        for (const Insert& in : t.inserts) ins.push(insert_value(in));
        Value& ps = tv.set("playlists", Value::array());
        for (const auto& p : t.playlists) ps.push(index_value(playlist(*p)));
        tracks_.push(std::move(tv));
        return ix;
    }
    std::size_t playlist(const Playlist& p) {
        bool added = false;
        const std::size_t ix = playlist_ix_.index(&p, added);
        if (added) playlists_.push(playlist_value(p));
        return ix;
    }
    std::size_t markers(const MarkerList& m) {
        bool added = false;
        const std::size_t ix = marker_ix_.index(&m, added);
        if (added) {
            Value& ml = markers_.push(Value::array());
            for (const Marker& k : m.markers) ml.push(marker_value(k));
        }
        return ix;
    }

    Table<Playlist> playlist_ix_;
    Table<TrackEdit> track_ix_;
    Table<MarkerList> marker_ix_;
    Table<EditState> edit_ix_;
};

bool window_value(const History& h, Value& root, std::string& error) {
    const Snapshot& cur = h.current();
    if (cur.edit != h.step_edit(h.undo_depth())) {
        error = "the current state is not the one at the undo pointer";
        return false;
    }
    WindowWriter w;
    Value steps = Value::array();
    for (std::size_t i = 0; i < h.step_count(); ++i) {
        Value sv = Value::object();
        sv.set("revision", integer(static_cast<std::int64_t>(h.step_revision(i))));
        sv.set("edit", index_value(w.edit(*h.step_edit(i))));
        sv.set("label", Value::string(h.step_label(i)));
        steps.push(std::move(sv));
    }

    root = Value::object();
    root.set("window_format", integer(kWindowFormat));
    root.set("revision", integer(static_cast<std::int64_t>(cur.revision)));
    root.set("next_id", id_value(cur.next_id));
    root.set("position", index_value(h.undo_depth()));
    root.set("settings", settings_value(cur.settings));
    root.set("bus", bus_value(cur.mix.bus));
    Value& mix = root.set("mix", Value::array());
    for (const TrackMix& m : cur.mix.tracks) mix.push(mix_value(m));
    Value& insert_mix = root.set("insert_mix", Value::array());
    for (const InsertMix& m : cur.mix.inserts) insert_mix.push(insert_mix_value(m));
    Value& sources = root.set("sources", Value::array());
    if (cur.sources) {
        for (const Source& src : cur.sources->sources) sources.push(source_value(src));
    }
    root.set("playlists", std::move(w.playlists_));
    root.set("tracks", std::move(w.tracks_));
    root.set("markers", std::move(w.markers_));
    root.set("edits", std::move(w.edits_));
    root.set("steps", std::move(steps));
    return true;
}

// An index into a table already read: only backwards, so nothing refers
// to itself or to what is not there yet.
bool table_index(Reader& rd, const Value& e, const std::string& p, std::size_t size,
                 std::size_t& out) {
    std::int64_t n = 0;
    if (!e.is_number() || !e.is_integer_lexeme() || !e.to_int64(n) || n < 0 ||
        static_cast<std::uint64_t>(n) >= size) {
        return rd.fail(p, "must be an index below " + std::to_string(size));
    }
    out = static_cast<std::size_t>(n);
    return true;
}

bool read_playlists(Reader& rd, const Value& root,
                    std::vector<std::shared_ptr<const Playlist>>& out) {
    const Value* list = nullptr;
    if (!rd.array(root, "", "playlists", list)) return false;
    for (const Value& e : list->items()) {
        auto p = std::make_shared<Playlist>();
        const Value* rs = nullptr;
        const Value* xs = nullptr;
        if (!rd.keys(e, "playlists", {"id", "name", "regions", "crossfades"}) ||
            !rd.id(e, "playlists", "id", p->id) || !rd.text(e, "playlists", "name", p->name) ||
            !rd.array(e, "playlists", "regions", rs) ||
            !rd.array(e, "playlists", "crossfades", xs)) {
            return false;
        }
        for (const Value& r : rs->items()) {
            Region region;
            if (!read_region_value(rd, r, "playlists.regions", region)) return false;
            p->regions.push_back(std::move(region));
        }
        for (const Value& x : xs->items()) {
            Crossfade c;
            if (!read_crossfade(rd, x, "playlists.crossfades", c)) return false;
            p->crossfades.push_back(c);
        }
        out.push_back(std::move(p));
    }
    return true;
}

bool read_tracks(Reader& rd, const Value& root,
                 const std::vector<std::shared_ptr<const Playlist>>& playlists,
                 std::vector<std::shared_ptr<const TrackEdit>>& out) {
    const Value* list = nullptr;
    if (!rd.array(root, "", "tracks", list)) return false;
    for (const Value& e : list->items()) {
        auto t = std::make_shared<TrackEdit>();
        const Value* ps = nullptr;
        const Value* ins = nullptr;
        if (!rd.keys(e, "tracks",
                     {"id", "name", "channels", "active_playlist", "inserts", "playlists"}) ||
            !rd.id(e, "tracks", "id", t->id) || !rd.text(e, "tracks", "name", t->name) ||
            !rd.small(e, "tracks", "channels", t->channels) ||
            !rd.id(e, "tracks", "active_playlist", t->active_playlist) ||
            !rd.array(e, "tracks", "inserts", ins) || !rd.array(e, "tracks", "playlists", ps)) {
            return false;
        }
        for (const Value& i : ins->items()) {
            Insert in;
            if (!read_insert_value(rd, i, "tracks.inserts", in)) return false;
            t->inserts.push_back(std::move(in));
        }
        for (const Value& i : ps->items()) {
            std::size_t ix = 0;
            if (!table_index(rd, i, "tracks.playlists", playlists.size(), ix)) return false;
            t->playlists.push_back(playlists[ix]);
        }
        out.push_back(std::move(t));
    }
    return true;
}

bool read_marker_lists(Reader& rd, const Value& root,
                       std::vector<std::shared_ptr<const MarkerList>>& out) {
    const Value* list = nullptr;
    if (!rd.array(root, "", "markers", list)) return false;
    for (const Value& e : list->items()) {
        if (!e.is_array()) return rd.fail("markers", "must hold arrays of markers");
        auto ml = std::make_shared<MarkerList>();
        for (const Value& m : e.items()) {
            Marker marker;
            if (!read_marker_value(rd, m, "markers", marker)) return false;
            ml->markers.push_back(std::move(marker));
        }
        out.push_back(std::move(ml));
    }
    return true;
}

bool read_edits(Reader& rd, const Value& root,
                const std::vector<std::shared_ptr<const TrackEdit>>& tracks,
                const std::vector<std::shared_ptr<const MarkerList>>& markers,
                std::vector<std::shared_ptr<const EditState>>& out) {
    const Value* list = nullptr;
    if (!rd.array(root, "", "edits", list)) return false;
    for (const Value& e : list->items()) {
        auto es = std::make_shared<EditState>();
        const Value* ts = nullptr;
        if (!rd.keys(e, "edits", {"tracks", "markers"}) || !rd.array(e, "edits", "tracks", ts)) {
            return false;
        }
        for (const Value& i : ts->items()) {
            std::size_t ix = 0;
            if (!table_index(rd, i, "edits.tracks", tracks.size(), ix)) return false;
            es->tracks.push_back(tracks[ix]);
        }
        const Value& mv = *e.find("markers");
        if (!mv.is_null()) {
            std::size_t ix = 0;
            if (!table_index(rd, mv, "edits.markers", markers.size(), ix)) return false;
            es->markers = markers[ix];
        }
        out.push_back(std::move(es));
    }
    return true;
}

bool read_window(Reader& rd, const Value& root, std::optional<History>& out) {
    // The format first: another version's keys are not this one's to judge.
    if (!root.is_object() || root.find("window_format") == nullptr) {
        return rd.fail("", "missing required key \"window_format\"");
    }
    std::int64_t format = 0;
    if (!rd.integer(root, "", "window_format", kMaxInteger, format)) return false;
    if (format != kWindowFormat) {
        return rd.fail("window_format", "is " + std::to_string(format) +
                                            " where this version reads " +
                                            std::to_string(kWindowFormat));
    }
    if (!rd.keys(root, "", {"window_format", "revision", "next_id", "position", "settings",
                            "bus", "mix", "insert_mix", "sources", "playlists", "tracks",
                            "markers", "edits", "steps"})) {
        return false;
    }
    Snapshot cur;
    std::int64_t revision = 0, position = 0;
    if (!rd.integer(root, "", "revision", kMaxInteger, revision) ||
        !rd.id(root, "", "next_id", cur.next_id) ||
        !rd.integer(root, "", "position", kMaxInteger, position) ||
        !read_settings(rd, root, cur.settings) || !read_bus(rd, root, cur.mix.bus)) {
        return false;
    }
    cur.revision = static_cast<std::uint64_t>(revision);

    const Value* list = nullptr;
    if (!rd.array(root, "", "mix", list)) return false;
    for (const Value& e : list->items()) {
        TrackMix m;
        if (!read_mix_value(rd, e, "mix", m)) return false;
        cur.mix.tracks.push_back(std::move(m));
    }
    if (!rd.array(root, "", "insert_mix", list)) return false;
    for (const Value& e : list->items()) {
        InsertMix m;
        if (!read_insert_mix_value(rd, e, "insert_mix", m)) return false;
        cur.mix.inserts.push_back(std::move(m));
    }
    auto pool = std::make_shared<SourcePool>();
    if (!rd.array(root, "", "sources", list)) return false;
    for (const Value& e : list->items()) {
        Source src;
        if (!read_source_value(rd, e, "sources", src)) return false;
        pool->sources.push_back(std::move(src));
    }
    cur.sources = std::move(pool);

    std::vector<std::shared_ptr<const Playlist>> playlists;
    std::vector<std::shared_ptr<const TrackEdit>> tracks;
    std::vector<std::shared_ptr<const MarkerList>> markers;
    std::vector<std::shared_ptr<const EditState>> edits;
    if (!read_playlists(rd, root, playlists) || !read_tracks(rd, root, playlists, tracks) ||
        !read_marker_lists(rd, root, markers) || !read_edits(rd, root, tracks, markers, edits)) {
        return false;
    }

    std::vector<History::WindowStep> steps;
    if (!rd.array(root, "", "steps", list)) return false;
    for (const Value& e : list->items()) {
        History::WindowStep st;
        std::int64_t rev = 0;
        std::size_t ix = 0;
        if (!rd.keys(e, "steps", {"revision", "edit", "label"}) ||
            !rd.integer(e, "steps", "revision", kMaxInteger, rev) ||
            !rd.text(e, "steps", "label", st.label) ||
            !table_index(rd, *e.find("edit"), "steps.edit", edits.size(), ix)) {
            return false;
        }
        st.revision = static_cast<std::uint64_t>(rev);
        st.edit = edits[ix];
        steps.push_back(std::move(st));
    }
    if (static_cast<std::uint64_t>(position) >= steps.size()) {
        return rd.fail("position", "is past the last undo step");
    }
    cur.edit = steps[static_cast<std::size_t>(position)].edit;

    std::string why;
    if (!check_invariants(cur, why)) return rd.fail("", "the current state: " + why);
    if (!History::restore(std::move(cur), std::move(steps), static_cast<std::size_t>(position),
                          out, why)) {
        return rd.fail("steps", why);
    }
    return true;
}

}  // namespace

bool encode_window(const History& history, std::string& out, std::string& error) {
    Value root;
    if (!window_value(history, root, error)) return false;
    return json::write(root, out, error);
}

bool decode_window(std::string_view bytes, std::optional<History>& out, std::string& error) {
    // The engine's bytes, already held to the wire's 64 MiB cap. A window
    // has more, smaller values per byte than a session file, so the value
    // and DOM caps are raised to what 64 MiB of it can hold.
    json::Limits limits;
    limits.max_values = std::size_t{16} << 20;
    limits.max_dom_bytes = std::size_t{1} << 30;
    Value root;
    json::Error perr;
    if (!json::parse(bytes, root, perr, limits)) {
        error = "undo window: line " + std::to_string(perr.line) + ", column " +
                std::to_string(perr.column) + ": " + perr.message;
        return false;
    }
    Reader rd(error);
    std::optional<History> decoded;
    if (!read_window(rd, root, decoded)) {
        error = "undo window: " + error;
        return false;
    }
    out = std::move(decoded);
    return true;
}

bool encode_operation(const Operation& op, std::string& out, std::string& error) {
    const Value v = std::visit(Encoder{}, op);
    return json::write(v, out, error);
}

bool decode_operation(std::string_view bytes, Operation& out, std::string& error) {
    Value root;
    json::Error perr;
    if (!json::parse(bytes, root, perr)) {
        error = "operation: line " + std::to_string(perr.line) + ", column " +
                std::to_string(perr.column) + ": " + perr.message;
        return false;
    }
    if (!root.is_object() || root.find("op") == nullptr || !root.find("op")->is_string()) {
        error = "operation: must be an object whose \"op\" names the operation";
        return false;
    }
    Reader rd(error);
    Operation decoded;
    if (!decode_value(rd, root, root.find("op")->text(), decoded)) {
        error = "operation " + root.find("op")->text() + ": " + error;
        return false;
    }
    out = std::move(decoded);
    return true;
}

namespace {

const char* const kNames[] = {
    "AddSource",    "SetMix",       "SetMonitorBus",   "SetSettings",
    "AddTrack",     "PlaceRegion",  "RecordTake",      "MoveRegion",
    "TrimRegion",   "SplitRegion",  "RemoveRegion",    "SetFade",
    "SetCrossfade", "RemoveCrossfade", "AddPlaylist",  "SetActivePlaylist",
    "AddMarker",    "MoveMarker",   "RemoveMarker",    "SetTrackName",
    "ReorderTracks", "AddInsert",   "RemoveInsert",    "MoveInsert",
    "SetInsertActive", "ReplaceInsert", "SetInsertBypass", "SetInsertParams",
    "SetInsertState", "Compound"};
static_assert(sizeof kNames / sizeof kNames[0] == std::variant_size_v<Operation>,
              "a name for every operation");

}  // namespace

const char* operation_name(const Operation& op) { return kNames[op.index()]; }

// Member's alternatives are Operation's, at the same indices.
const char* member_name(const Member& m) { return kNames[m.index()]; }

std::string display_name(const Operation& op) {
    const auto* c = std::get_if<Compound>(&op);
    if (c == nullptr) return operation_name(op);
    if (!c->label.empty()) return c->label;
    for (const Member& m : c->ops) {
        if (member_is_undoable(m)) return member_name(m);
    }
    return c->ops.empty() ? "Compound" : member_name(c->ops.front());
}

}  // namespace sonora::model
