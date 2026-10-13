// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/model/op_codec.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <type_traits>
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
};

// --- reading ----------------------------------------------------------------

class Reader {
public:
    explicit Reader(std::string& error) : error_(error) {}

    bool fail(const std::string& path, const std::string& msg) {
        error_ = path.empty() ? msg : path + ": " + msg;
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
};

bool read_fade(Reader& rd, const Value& obj, const std::string& path, const char* key, Fade& f) {
    const Value& v = *obj.find(key);
    const std::string p = Reader::at(path, key);
    return rd.keys(v, p, {"length", "shape"}) && rd.frames(v, p, "length", f.length) &&
           rd.choice(v, p, "shape", kShapes, f.shape);
}

bool read_region(Reader& rd, const Value& obj, const std::string& path, const char* key,
                 Region& r) {
    const Value& v = *obj.find(key);
    const std::string p = Reader::at(path, key);
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

bool read_crossfade(Reader& rd, const Value& v, const std::string& p, Crossfade& x) {
    return rd.keys(v, p, {"left", "right", "length", "align", "shape"}) &&
           rd.id(v, p, "left", x.left) && rd.id(v, p, "right", x.right) &&
           rd.frames(v, p, "length", x.length) && rd.choice(v, p, "align", kAligns, x.align) &&
           rd.choice(v, p, "shape", kShapes, x.shape);
}

bool read_source(Reader& rd, const Value& obj, const std::string& path, Source& s) {
    const Value& v = *obj.find("source");
    const std::string p = Reader::at(path, "source");
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

bool read_mix(Reader& rd, const Value& obj, const std::string& path, TrackMix& m) {
    const Value& v = *obj.find("mix");
    const std::string p = Reader::at(path, "mix");
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

bool read_marker(Reader& rd, const Value& obj, Marker& m) {
    const Value& v = *obj.find("marker");
    return rd.keys(v, "marker", {"id", "position", "name", "length"}) &&
           rd.id(v, "marker", "id", m.id) && rd.frames(v, "marker", "position", m.position) &&
           rd.text(v, "marker", "name", m.name) && rd.frames(v, "marker", "length", m.length);
}

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
    } else {
        return rd.fail("op", "\"" + kind + "\" is not an operation this version knows");
    }
    return true;
}

}  // namespace

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

const char* operation_name(const Operation& op) {
    static const char* const kNames[] = {
        "AddSource",    "SetMix",       "SetMonitorBus",   "SetSettings",
        "AddTrack",     "PlaceRegion",  "RecordTake",      "MoveRegion",
        "TrimRegion",   "SplitRegion",  "RemoveRegion",    "SetFade",
        "SetCrossfade", "RemoveCrossfade", "AddPlaylist",  "SetActivePlaylist",
        "AddMarker",    "MoveMarker",   "RemoveMarker",    "SetTrackName",
        "ReorderTracks"};
    static_assert(sizeof kNames / sizeof kNames[0] == std::variant_size_v<Operation>,
                  "a name for every operation");
    return kNames[op.index()];
}

}  // namespace sonora::model
