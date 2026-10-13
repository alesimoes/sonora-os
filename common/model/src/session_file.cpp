// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/model/session_file.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <unordered_map>
#include <utility>

#include "checks.h"
#include "sonora/json/json.h"

namespace sonora::model {

namespace {

using json::Value;

// --- enumerations, as the file spells them ---------------------------------

template <typename E>
struct Name {
    const char* text;
    E value;
};

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

template <typename E, std::size_t N>
const char* name_of(const Name<E> (&table)[N], E v) {
    for (const auto& n : table) {
        if (n.value == v) return n.text;
    }
    return table[0].text;
}

// --- migration --------------------------------------------------------------

// kMigrations[v - 1] turns a version-v tree into version v + 1, in place: a
// pure function on the parsed tree (session-format.md, "Versioning"). Each
// gets a frozen fixture in common/model/tests/fixtures that must load
// forever. Version 1 is the first, so there are none yet.
using Migration = bool (*)(Value& root, std::string& error);
const std::array<Migration, kFormatVersion - 1> kMigrations{};

bool migrate(Value& root, std::int64_t from, std::string& error) {
    for (std::int64_t v = from; v < kFormatVersion; ++v) {
        if (!kMigrations[static_cast<std::size_t>(v - 1)](root, error)) return false;
    }
    return true;
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
    v.set("shape", Value::string(name_of(kShapes, f.shape)));
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

Value source_value(const Source& s) {
    Value v = Value::object();
    v.set("id", id_value(s.id));
    v.set("path", Value::string(s.path));
    v.set("channels", integer(s.channels));
    v.set("length", integer(s.length));
    v.set("format", Value::string(name_of(kFormats, s.format)));
    v.set("time_reference", integer(s.time_reference));
    if (!s.umid.empty()) v.set("umid", Value::string(s.umid));
    v.set("kind", Value::string(name_of(kKinds, s.kind)));
    if (s.kind == SourceKind::Recorded) {
        v.set("take", integer(s.take));
        v.set("recorded_on_track", id_value(s.recorded_on_track));
        v.set("latency_compensation", integer(s.latency_compensation));
    }
    return v;
}

// --- reading ----------------------------------------------------------------

std::string index_path(const std::string& path, std::size_t i) {
    return path + "[" + std::to_string(i) + "]";
}

std::string key_path(const std::string& path, const char* key) {
    return path.empty() ? std::string(key) : path + "." + key;
}

// Finds the node a checker's relative path ("regions[3]", "tempo[1]",
// "transport.loop") names, so its error can carry a line.
const Value* node_at(const Value* base, const std::string& where) {
    std::size_t i = 0;
    while (base != nullptr && i < where.size()) {
        if (where[i] == '.') {
            ++i;
            continue;
        }
        if (where[i] == '[') {
            const std::size_t close = where.find(']', i);
            const std::size_t n = std::stoul(where.substr(i + 1, close - i - 1));
            base = base->is_array() && n < base->items().size() ? &base->items()[n]
                                                                : nullptr;
            i = close + 1;
            continue;
        }
        const std::size_t end = where.find_first_of(".[", i);
        const std::string key = where.substr(i, end == std::string::npos ? end : end - i);
        base = base->is_object() ? base->find(key) : nullptr;
        i = end == std::string::npos ? where.size() : end;
    }
    return base;
}

class Reader {
public:
    explicit Reader(LoadError& error) : error_(error) {}

    bool fail(const Value& at, const std::string& path, const std::string& msg) {
        error_.message = msg;
        error_.path = path;
        error_.line = at.line();
        error_.column = at.column();
        return false;
    }

    // The set of keys is strict; their order is not (session-format.md).
    bool keys(const Value& v, const std::string& path,
              std::initializer_list<const char*> required,
              std::initializer_list<const char*> optional = {}) {
        if (!v.is_object()) return fail(v, path, "must be an object");
        for (const auto& kv : v.members()) {
            bool known = false;
            for (const char* k : required) known = known || kv.first == k;
            for (const char* k : optional) known = known || kv.first == k;
            if (!known) {
                return fail(kv.second, key_path(path, kv.first.c_str()),
                            "unknown key \"" + kv.first + "\"");
            }
        }
        for (const char* k : required) {
            if (v.find(k) == nullptr) {
                return fail(v, path, std::string("missing required key \"") + k + "\"");
            }
        }
        return true;
    }

    // Integers: no fraction, no exponent, never through a double, and inside
    // 0 .. 2^53 - 1 unless the field is narrower.
    bool integer(const Value& obj, const std::string& path, const char* key,
                 std::int64_t max, std::int64_t& out) {
        const Value& v = *obj.find(key);
        const std::string p = key_path(path, key);
        std::int64_t n = 0;
        if (!v.is_number()) return fail(v, p, "must be an integer");
        if (!v.is_integer_lexeme()) {
            return fail(v, p, "must be an integer, without a fraction or an exponent");
        }
        if (!v.to_int64(n) || n < 0 || n > max) {
            return fail(v, p, "must be an integer from 0 to " + std::to_string(max));
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

    // Straight into float with from_chars: no double in between, no locale.
    bool real(const Value& obj, const std::string& path, const char* key, float& out) {
        const Value& v = *obj.find(key);
        const std::string p = key_path(path, key);
        if (!v.is_number()) return fail(v, p, "must be a number");
        if (!v.to_float(out)) return fail(v, p, "is too large for a float");
        return true;
    }

    bool text(const Value& obj, const std::string& path, const char* key,
              std::string& out) {
        const Value& v = *obj.find(key);
        if (!v.is_string()) return fail(v, key_path(path, key), "must be a string");
        out = v.text();
        return true;
    }

    bool flag(const Value& obj, const std::string& path, const char* key, bool& out) {
        const Value& v = *obj.find(key);
        if (!v.is_bool()) return fail(v, key_path(path, key), "must be true or false");
        out = v.as_bool();
        return true;
    }

    template <typename E, std::size_t N>
    bool choice(const Value& obj, const std::string& path, const char* key,
                const Name<E> (&table)[N], E& out) {
        const Value& v = *obj.find(key);
        std::string all;
        for (const auto& n : table) {
            if (v.is_string() && v.text() == n.text) {
                out = n.value;
                return true;
            }
            all += (all.empty() ? "\"" : ", \"") + std::string(n.text) + "\"";
        }
        return fail(v, key_path(path, key), "must be one of " + all);
    }

    bool array(const Value& obj, const std::string& path, const char* key,
               const Value*& out) {
        const Value& v = *obj.find(key);
        if (!v.is_array()) return fail(v, key_path(path, key), "must be an array");
        out = &v;
        return true;
    }

    bool range(const Value& obj, const std::string& path, const char* key,
               std::array<Frames, 2>& out) {
        const Value& v = *obj.find(key);
        const std::string p = key_path(path, key);
        if (!v.is_array() || v.items().size() != 2) {
            return fail(v, p, "must be [start, end]");
        }
        for (std::size_t i = 0; i < 2; ++i) {
            const Value& e = v.items()[i];
            std::int64_t n = 0;
            if (!e.is_integer_lexeme() || !e.to_int64(n) || n < 0 || n > kMaxInteger) {
                return fail(e, index_path(p, i),
                            "must be an integer from 0 to " + std::to_string(kMaxInteger));
            }
            out[i] = n;
        }
        return true;
    }

    // Ids share one space across sources, tracks, playlists, regions and
    // markers (session-format.md, "Ids").
    bool claim(Id id, const Value& at, const std::string& path) {
        if (id == 0) return fail(at, path + ".id", "must be at least 1");
        const auto r = ids_.emplace(id, path);
        if (!r.second) {
            return fail(at, path + ".id",
                        "id " + std::to_string(id) + " is already used by " + r.first->second);
        }
        max_id_ = std::max(max_id_, id);
        return true;
    }

    bool checked(const std::string& why, const Value& base, const std::string& path,
                 const std::string& where) {
        if (why.empty()) return true;
        const Value* at = where.empty() ? &base : node_at(&base, where);
        return fail(at != nullptr ? *at : base,
                    where.empty() ? path : (path.empty() ? where : path + "." + where), why);
    }

    Id max_id() const { return max_id_; }

private:
    LoadError& error_;
    std::unordered_map<Id, std::string> ids_;
    Id max_id_ = 0;
};

bool read_settings(Reader& rd, const Value& root, SessionSettings& s) {
    if (!rd.text(root, "", "name", s.name)) return false;
    std::int64_t rate = 0;
    if (!rd.integer(root, "", "sample_rate", 0xFFFFFFFF, rate)) return false;
    s.sample_rate = static_cast<std::uint32_t>(rate);
    if (!rd.choice(root, "", "audio_format", kFormats, s.audio_format)) return false;

    const Value& tc = *root.find("timecode");
    if (!rd.keys(tc, "timecode", {"origin", "fps"})) return false;
    if (!rd.integer(tc, "timecode", "origin", kMaxInteger, s.timecode.origin)) return false;
    if (!rd.choice(tc, "timecode", "fps", kFps, s.timecode.fps)) return false;

    const Value* tempo = nullptr;
    if (!rd.array(root, "", "tempo", tempo)) return false;
    s.tempo.clear();
    for (std::size_t i = 0; i < tempo->items().size(); ++i) {
        const Value& e = tempo->items()[i];
        const std::string p = index_path("tempo", i);
        if (!rd.keys(e, p, {"at", "bpm", "meter"})) return false;
        TempoEntry t;
        if (!rd.frames(e, p, "at", t.at)) return false;
        if (!rd.real(e, p, "bpm", t.bpm)) return false;
        const Value& m = *e.find("meter");
        std::array<Frames, 2> meter{};
        if (!rd.range(e, p, "meter", meter)) return false;
        if (meter[0] > 0xFFFF || meter[1] > 0xFFFF) {
            return rd.fail(m, p + ".meter", "must be two counts from 1 to 65535");
        }
        t.meter = {{static_cast<std::uint32_t>(meter[0]),
                    static_cast<std::uint32_t>(meter[1])}};
        s.tempo.push_back(t);
    }

    const Value& tr = *root.find("transport");
    if (!rd.keys(tr, "transport",
                 {"loop", "punch", "pre_roll", "post_roll", "punch_crossfade"})) {
        return false;
    }
    if (!rd.range(tr, "transport", "loop", s.transport.loop)) return false;
    if (!rd.range(tr, "transport", "punch", s.transport.punch)) return false;
    if (!rd.frames(tr, "transport", "pre_roll", s.transport.pre_roll)) return false;
    if (!rd.frames(tr, "transport", "post_roll", s.transport.post_roll)) return false;
    if (!rd.frames(tr, "transport", "punch_crossfade", s.transport.punch_crossfade)) {
        return false;
    }
    std::string where;
    return rd.checked(detail::settings_problem(s, where), root, "", where);
}

bool read_bus(Reader& rd, const Value& root, MonitorBus& b) {
    const Value& v = *root.find("monitor_bus");
    const std::string p = "monitor_bus";
    if (!rd.keys(v, p, {"outputs", "gain_db", "pan_law_db"})) return false;
    std::array<Frames, 2> outs{};
    if (!rd.range(v, p, "outputs", outs)) return false;
    if (outs[0] > 0xFFFF || outs[1] > 0xFFFF) {
        return rd.fail(*v.find("outputs"), p + ".outputs",
                       "must be two channel indices from 0 to 65535");
    }
    b.outputs = {{static_cast<std::uint16_t>(outs[0]), static_cast<std::uint16_t>(outs[1])}};
    if (!rd.real(v, p, "gain_db", b.gain_db)) return false;
    if (!rd.real(v, p, "pan_law_db", b.pan_law_db)) return false;
    std::string where;
    return rd.checked(detail::bus_problem(b, where), v, p, where);
}

bool read_sources(Reader& rd, const Value& root, SourcePool& pool) {
    const Value* list = nullptr;
    if (!rd.array(root, "", "sources", list)) return false;
    for (std::size_t i = 0; i < list->items().size(); ++i) {
        const Value& v = list->items()[i];
        const std::string p = index_path("sources", i);
        if (!v.is_object()) return rd.fail(v, p, "must be an object");
        Source s;
        if (v.find("kind") == nullptr) {
            return rd.fail(v, p, "missing required key \"kind\"");
        }
        if (!rd.choice(v, p, "kind", kKinds, s.kind)) return false;
        const bool recorded = s.kind == SourceKind::Recorded;
        const bool ok =
            recorded ? rd.keys(v, p,
                               {"id", "path", "channels", "length", "format",
                                "time_reference", "kind", "take", "recorded_on_track",
                                "latency_compensation"},
                               {"umid"})
                     : rd.keys(v, p,
                               {"id", "path", "channels", "length", "format",
                                "time_reference", "kind"},
                               {"umid"});
        if (!ok) return false;
        if (!rd.id(v, p, "id", s.id)) return false;
        if (!rd.text(v, p, "path", s.path)) return false;
        if (!rd.small(v, p, "channels", s.channels)) return false;
        if (!rd.frames(v, p, "length", s.length)) return false;
        if (!rd.choice(v, p, "format", kFormats, s.format)) return false;
        if (!rd.integer(v, p, "time_reference", kMaxInteger, s.time_reference)) return false;
        if (v.find("umid") != nullptr && !rd.text(v, p, "umid", s.umid)) return false;
        if (recorded) {
            if (!rd.integer(v, p, "take", kMaxInteger, s.take)) return false;
            if (!rd.id(v, p, "recorded_on_track", s.recorded_on_track)) return false;
            if (!rd.frames(v, p, "latency_compensation", s.latency_compensation)) {
                return false;
            }
        }
        if (!rd.checked(detail::source_problem(s), v, p, "")) return false;
        if (!pool.sources.empty() && s.id <= pool.sources.back().id) {
            return rd.fail(v, p + ".id", "sources must be sorted by id");
        }
        if (!rd.claim(s.id, v, p)) return false;
        pool.sources.push_back(std::move(s));
    }
    return true;
}

bool read_fade(Reader& rd, const Value& region, const std::string& path, const char* key,
               Fade& f) {
    const Value& v = *region.find(key);
    const std::string p = key_path(path, key);
    if (!rd.keys(v, p, {"length", "shape"})) return false;
    if (!rd.frames(v, p, "length", f.length)) return false;
    return rd.choice(v, p, "shape", kShapes, f.shape);
}

bool read_region(Reader& rd, const Value& v, const std::string& p, Region& r) {
    if (!rd.keys(v, p,
                 {"id", "name", "position", "length", "source", "source_offset",
                  "gain_db", "mute", "fade_in", "fade_out"})) {
        return false;
    }
    if (!rd.id(v, p, "id", r.id)) return false;
    if (!rd.text(v, p, "name", r.name)) return false;
    if (!rd.frames(v, p, "position", r.position)) return false;
    if (!rd.frames(v, p, "length", r.length)) return false;
    const Value* src = nullptr;
    if (!rd.array(v, p, "source", src)) return false;
    for (std::size_t c = 0; c < src->items().size(); ++c) {
        const Value& e = src->items()[c];
        const std::string sp = index_path(p + ".source", c);
        if (!rd.keys(e, sp, {"id", "channel"})) return false;
        SourceRef ref;
        if (!rd.id(e, sp, "id", ref.source)) return false;
        if (!rd.small(e, sp, "channel", ref.channel)) return false;
        r.channels.push_back(ref);
    }
    if (!rd.frames(v, p, "source_offset", r.source_offset)) return false;
    if (!rd.real(v, p, "gain_db", r.gain_db)) return false;
    if (!rd.flag(v, p, "mute", r.mute)) return false;
    if (!read_fade(rd, v, p, "fade_in", r.fade_in)) return false;
    return read_fade(rd, v, p, "fade_out", r.fade_out);
}

bool read_playlist(Reader& rd, const Value& v, const std::string& p,
                   const SourcePool& pool, std::uint16_t channels, Playlist& pl) {
    if (!rd.keys(v, p, {"id", "name", "regions", "crossfades"})) return false;
    if (!rd.id(v, p, "id", pl.id)) return false;
    if (!rd.text(v, p, "name", pl.name)) return false;
    if (!rd.claim(pl.id, v, p)) return false;
    const Value* regions = nullptr;
    if (!rd.array(v, p, "regions", regions)) return false;
    for (std::size_t i = 0; i < regions->items().size(); ++i) {
        const Value& rv = regions->items()[i];
        const std::string rp = index_path(p + ".regions", i);
        Region r;
        if (!read_region(rd, rv, rp, r)) return false;
        if (!rd.claim(r.id, rv, rp)) return false;
        pl.regions.push_back(std::move(r));
    }
    const Value* xfades = nullptr;
    if (!rd.array(v, p, "crossfades", xfades)) return false;
    for (std::size_t i = 0; i < xfades->items().size(); ++i) {
        const Value& xv = xfades->items()[i];
        const std::string xp = index_path(p + ".crossfades", i);
        if (!rd.keys(xv, xp, {"left", "right", "length", "align", "shape"})) return false;
        Crossfade x;
        if (!rd.id(xv, xp, "left", x.left)) return false;
        if (!rd.id(xv, xp, "right", x.right)) return false;
        if (!rd.frames(xv, xp, "length", x.length)) return false;
        if (!rd.choice(xv, xp, "align", kAligns, x.align)) return false;
        if (!rd.choice(xv, xp, "shape", kShapes, x.shape)) return false;
        pl.crossfades.push_back(x);
    }
    std::string where;
    return rd.checked(detail::playlist_problem(pl, pool, channels, where), v, p, where);
}

bool read_tracks(Reader& rd, const Value& root, const SourcePool& pool, EditState& edit,
                 MixState& mix) {
    const Value* list = nullptr;
    if (!rd.array(root, "", "tracks", list)) return false;
    for (std::size_t i = 0; i < list->items().size(); ++i) {
        const Value& v = list->items()[i];
        const std::string p = index_path("tracks", i);
        if (!rd.keys(v, p,
                     {"id", "name", "channels", "input", "monitor", "gain_db", "pan",
                      "mute", "solo", "active_playlist", "playlists"})) {
            return false;
        }
        auto t = std::make_shared<TrackEdit>();
        TrackMix m;
        if (!rd.id(v, p, "id", t->id)) return false;
        if (!rd.claim(t->id, v, p)) return false;
        m.track = t->id;
        if (!rd.text(v, p, "name", t->name)) return false;
        if (!rd.checked(detail::valid_text(t->name) ? "" : "is not valid UTF-8", v,
                        p + ".name", "")) {
            return false;
        }
        if (!rd.small(v, p, "channels", t->channels)) return false;
        if (t->channels < 1 || t->channels > 2) {
            return rd.fail(*v.find("channels"), p + ".channels",
                           "must be 1 or 2 in this version");
        }
        const Value* input = nullptr;
        if (!rd.array(v, p, "input", input)) return false;
        for (std::size_t c = 0; c < input->items().size(); ++c) {
            const Value& e = input->items()[c];
            std::int64_t n = 0;
            if (!e.is_integer_lexeme() || !e.to_int64(n) || n < 0 || n > 0xFFFF) {
                return rd.fail(e, index_path(p + ".input", c),
                               "must be a capture channel index from 0 to 65535");
            }
            m.input.push_back(static_cast<std::uint16_t>(n));
        }
        if (!rd.choice(v, p, "monitor", kMonitors, m.monitor)) return false;
        if (!rd.real(v, p, "gain_db", m.gain_db)) return false;
        if (!rd.real(v, p, "pan", m.pan)) return false;
        if (!rd.flag(v, p, "mute", m.mute)) return false;
        if (!rd.flag(v, p, "solo", m.solo)) return false;
        std::string where;
        if (!rd.checked(detail::mix_problem(m, t->channels, where), v, p, where)) {
            return false;
        }
        if (!rd.id(v, p, "active_playlist", t->active_playlist)) return false;
        const Value* pls = nullptr;
        if (!rd.array(v, p, "playlists", pls)) return false;
        if (pls->items().empty()) {
            return rd.fail(*pls, p + ".playlists", "a track needs at least one playlist");
        }
        bool active = false;
        for (std::size_t k = 0; k < pls->items().size(); ++k) {
            Playlist pl;
            if (!read_playlist(rd, pls->items()[k], index_path(p + ".playlists", k), pool,
                               t->channels, pl)) {
                return false;
            }
            active = active || pl.id == t->active_playlist;
            t->playlists.push_back(std::make_shared<const Playlist>(std::move(pl)));
        }
        if (!active) {
            return rd.fail(*v.find("active_playlist"), p + ".active_playlist",
                           "names no playlist of this track");
        }
        edit.tracks.push_back(std::move(t));
        mix.tracks.push_back(std::move(m));
    }
    return true;
}

bool read_markers(Reader& rd, const Value& root, MarkerList& out) {
    const Value* list = nullptr;
    if (!rd.array(root, "", "markers", list)) return false;
    for (std::size_t i = 0; i < list->items().size(); ++i) {
        const Value& v = list->items()[i];
        const std::string p = index_path("markers", i);
        if (!rd.keys(v, p, {"id", "position", "name", "length"})) return false;
        Marker m;
        if (!rd.id(v, p, "id", m.id)) return false;
        if (!rd.frames(v, p, "position", m.position)) return false;
        if (!rd.text(v, p, "name", m.name)) return false;
        if (!rd.frames(v, p, "length", m.length)) return false;
        if (!rd.checked(detail::marker_problem(m), v, p, "")) return false;
        if (!out.markers.empty()) {
            const Marker& prev = out.markers.back();
            if (m.position < prev.position ||
                (m.position == prev.position && m.id < prev.id)) {
                return rd.fail(v, p + ".position", "markers must be sorted by position");
            }
        }
        if (!rd.claim(m.id, v, p)) return false;
        out.markers.push_back(std::move(m));
    }
    return true;
}

}  // namespace

std::string LoadError::text() const {
    std::string s;
    if (line > 0) {
        s = "line " + std::to_string(line) + ", column " + std::to_string(column) + ": ";
    }
    if (!path.empty()) s += path + ": ";
    return s + message;
}

bool serialize(const Snapshot& s, const WriteInfo& info, std::string& out,
               std::string& error) {
    std::string why;
    if (!check_invariants(s, why)) {
        error = "refusing to write a session that would not load: " + why;
        return false;
    }
    if (info.save_generation > static_cast<std::uint64_t>(kMaxInteger)) {
        error = "save_generation is out of range";
        return false;
    }
    Value root = Value::object();
    root.set("sonora_session", integer(kFormatVersion));
    root.set("written_by", Value::string(info.written_by));
    root.set("save_generation", integer(static_cast<std::int64_t>(info.save_generation)));
    const SessionSettings& st = s.settings;
    root.set("name", Value::string(st.name));
    root.set("sample_rate", integer(st.sample_rate));
    root.set("audio_format", Value::string(name_of(kFormats, st.audio_format)));
    Value& tc = root.set("timecode", Value::object());
    tc.set("origin", integer(st.timecode.origin));
    tc.set("fps", Value::string(name_of(kFps, st.timecode.fps)));
    Value& tempo = root.set("tempo", Value::array());
    for (const TempoEntry& t : st.tempo) {
        Value& e = tempo.push(Value::object());
        e.set("at", integer(t.at));
        e.set("bpm", Value::number_float(t.bpm));
        e.set("meter", pair(t.meter[0], t.meter[1]));
    }
    Value& tr = root.set("transport", Value::object());
    tr.set("loop", pair(st.transport.loop[0], st.transport.loop[1]));
    tr.set("punch", pair(st.transport.punch[0], st.transport.punch[1]));
    tr.set("pre_roll", integer(st.transport.pre_roll));
    tr.set("post_roll", integer(st.transport.post_roll));
    tr.set("punch_crossfade", integer(st.transport.punch_crossfade));
    Value& bus = root.set("monitor_bus", Value::object());
    bus.set("outputs", pair(s.mix.bus.outputs[0], s.mix.bus.outputs[1]));
    bus.set("gain_db", Value::number_float(s.mix.bus.gain_db));
    bus.set("pan_law_db", Value::number_float(s.mix.bus.pan_law_db));

    Value& sources = root.set("sources", Value::array());
    for (const Source& src : s.sources->sources) sources.push(source_value(src));

    Value& tracks = root.set("tracks", Value::array());
    for (const auto& tp : s.edit->tracks) {
        const TrackEdit& t = *tp;
        const TrackMix& m = *s.mix.find(t.id);   // check_invariants guarantees it
        Value& tv = tracks.push(Value::object());
        tv.set("id", id_value(t.id));
        tv.set("name", Value::string(t.name));
        tv.set("channels", integer(t.channels));
        Value& input = tv.set("input", Value::array());
        for (const std::uint16_t c : m.input) input.push(integer(c));
        tv.set("monitor", Value::string(name_of(kMonitors, m.monitor)));
        tv.set("gain_db", Value::number_float(m.gain_db));
        tv.set("pan", Value::number_float(m.pan));
        tv.set("mute", Value::boolean(m.mute));
        tv.set("solo", Value::boolean(m.solo));
        tv.set("active_playlist", id_value(t.active_playlist));
        Value& pls = tv.set("playlists", Value::array());
        for (const auto& pp : t.playlists) {
            Value& pv = pls.push(Value::object());
            pv.set("id", id_value(pp->id));
            pv.set("name", Value::string(pp->name));
            Value& regions = pv.set("regions", Value::array());
            for (const Region& r : pp->regions) regions.push(region_value(r));
            Value& xfades = pv.set("crossfades", Value::array());
            for (const Crossfade& x : pp->crossfades) {
                Value& xv = xfades.push(Value::object());
                xv.set("left", id_value(x.left));
                xv.set("right", id_value(x.right));
                xv.set("length", integer(x.length));
                xv.set("align", Value::string(name_of(kAligns, x.align)));
                xv.set("shape", Value::string(name_of(kShapes, x.shape)));
            }
        }
    }

    Value& markers = root.set("markers", Value::array());
    for (const Marker& m : s.edit->markers->markers) {
        Value& mv = markers.push(Value::object());
        mv.set("id", id_value(m.id));
        mv.set("position", integer(m.position));
        mv.set("name", Value::string(m.name));
        mv.set("length", integer(m.length));
    }
    return json::write(root, out, error);
}

bool deserialize(std::string_view bytes, Loaded& out, LoadError& error) {
    error = LoadError{};
    Value root;
    json::Error perr;
    if (!json::parse(bytes, root, perr)) {
        error.message = perr.message;
        error.line = perr.line;
        error.column = perr.column;
        return false;
    }
    Reader rd(error);
    // The magic. Anything else is not a session file, whatever it holds.
    if (!root.is_object() || root.members().empty() ||
        root.members().front().first != "sonora_session") {
        return rd.fail(root, "",
                       "not a Sonora session: \"sonora_session\" must be the first key");
    }
    const Value& version = root.members().front().second;
    std::int64_t file_version = 0;
    if (!version.is_integer_lexeme()) {
        return rd.fail(version, "sonora_session", "must be an integer format version");
    }
    // An integer past int64 is a version from far in the future (or below
    // zero, one that never existed), not a malformed file.
    const bool fits = version.to_int64(file_version);
    const bool negative = !version.text().empty() && version.text().front() == '-';
    // Checked before anything else is read: a newer file's keys are not
    // ours to judge, and a lenient read would show a session that is not
    // the one on disk.
    if (fits ? file_version > kFormatVersion : !negative) {
        return rd.fail(version, "sonora_session",
                       "this session is format version " + version.text() +
                           ", newer than this program, which reads up to version " +
                           std::to_string(kFormatVersion) +
                           "; open it with a newer Sonora");
    }
    if (!fits || file_version < 1) {
        return rd.fail(version, "sonora_session",
                       "format version " + version.text() +
                           " does not exist; versions start at 1");
    }
    std::string why;
    if (!migrate(root, file_version, why)) {
        error.message = "migrating from format version " + std::to_string(file_version) +
                        ": " + why;
        return false;
    }

    if (!rd.keys(root, "",
                 {"sonora_session", "written_by", "save_generation", "name",
                  "sample_rate", "audio_format", "timecode", "tempo", "transport",
                  "monitor_bus", "sources", "tracks", "markers"})) {
        return false;
    }
    Loaded loaded;
    loaded.file_version = file_version;
    if (!rd.text(root, "", "written_by", loaded.written_by)) return false;
    std::int64_t generation = 0;
    if (!rd.integer(root, "", "save_generation", kMaxInteger, generation)) return false;
    loaded.save_generation = static_cast<std::uint64_t>(generation);

    SessionSettings settings;
    if (!read_settings(rd, root, settings)) return false;
    MixState mix;
    if (!read_bus(rd, root, mix.bus)) return false;
    auto pool = std::make_shared<SourcePool>();
    if (!read_sources(rd, root, *pool)) return false;
    auto edit = std::make_shared<EditState>();
    if (!read_tracks(rd, root, *pool, *edit, mix)) return false;
    auto markers = std::make_shared<MarkerList>();
    if (!read_markers(rd, root, *markers)) return false;
    edit->markers = std::move(markers);

    Snapshot& s = loaded.snapshot;
    s.edit = std::move(edit);
    s.sources = std::move(pool);
    s.mix = std::move(mix);
    s.settings = std::move(settings);
    s.revision = 0;
    s.next_id = rd.max_id() + 1;
    // Every check above ran with a line; this one is the net under them,
    // the same function apply() answers to.
    if (!check_invariants(s, why)) {
        error.message = why;
        return false;
    }
    out = std::move(loaded);
    return true;
}

}  // namespace sonora::model
