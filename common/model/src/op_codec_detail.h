// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the op codec's translation units share (op_codec.h): the spellings,
// the value writers and readers, and one encoder and decoder per family of
// operations (ADR-0059), each in its own file so that none visits more than
// one short variant.
#pragma once

#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "checks.h"
#include "midi_json.h"
#include "sonora/json/json.h"
#include "sonora/model/op_codec.h"

namespace sonora::model::codec {

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
constexpr Name<TrackKind> kTrackKinds[] = {{"audio", TrackKind::Audio},
                                           {"midi", TrackKind::Midi},
                                           {"instrument", TrackKind::Instrument}};
constexpr Name<Timebase> kTimebases[] = {{"frames", Timebase::Frames},
                                         {"ticks", Timebase::Ticks}};
constexpr Name<AutomationMode> kAutomationModes[] = {
    {"off", AutomationMode::Off},     {"read", AutomationMode::Read},
    {"touch", AutomationMode::Touch}, {"latch", AutomationMode::Latch},
    {"write", AutomationMode::Write}, {"trim", AutomationMode::Trim},
    {"write_trim", AutomationMode::WriteTrim}};
constexpr Name<AutomationParam> kAutomationParams[] = {
    {"volume", AutomationParam::Volume},
    {"pan", AutomationParam::Pan},
    {"plugin", AutomationParam::Plugin},
    {"volume_trim", AutomationParam::VolumeTrim},
    {"mute", AutomationParam::Mute},
    {"send_level", AutomationParam::SendLevel},
    {"send_pan", AutomationParam::SendPan}};

template <typename E, std::size_t N>
Value name_of(const Name<E> (&table)[N], E v) {
    for (const auto& n : table) {
        if (n.value == v) return Value::string(n.text);
    }
    return Value::string(table[0].text);
}

// --- writing ----------------------------------------------------------------

Value integer(std::int64_t n);
Value id_value(Id id);

Value freeze_value(const PlaylistFreeze& f);

Value pair(std::int64_t a, std::int64_t b);

Value fade_value(const Fade& f);

Value gain_line_value(const std::vector<GainPoint>& line);

Value region_value(const Region& r);

Value crossfade_value(const Crossfade& x);

Value source_value(const Source& s);

Value port_value(const std::string& port, std::uint8_t channel);

Value mix_value(const TrackMix& m);

// [[at, value], ...]: the session file's spelling (ADR-0027 §6).
Value points_value(const std::vector<AutomationPoint>& points);

// "param", for a plugin lane its "insert" and "key", for a send lane its
// "send"; any other lane has none, so its encoding is a version-35 peer's.
void set_lane_target(Value& v, AutomationParam param, Id insert, Id send,
                     const std::string& key);

// Which keys `v` must carry beside "param", by the lane its param names.
enum class LaneKeys { Plain, Plugin, Send };
LaneKeys lane_keys(const Value& v);

Value lane_value(const AutomationLane& l);

Value midi_source_value(const MidiSource& s);

template <typename Notes>
Value notes_value(const Notes& notes) {
    Value v = Value::array();
    for (const midi::Note& n : notes) v.push(detail::note_row(n));
    return v;
}

template <typename Events>
Value events_value(const Events& events) {
    Value v = Value::array();
    for (const MidiEvent& e : events) v.push(detail::event_row(e));
    return v;
}

template <typename Sysex>
Value sysex_value(const Sysex& sysex) {
    Value v = Value::array();
    for (const MidiSysEx& x : sysex) v.push(detail::sysex_row(x));
    return v;
}

template <typename R>
Value removals_value(const std::vector<R>& removals, Value (*row)(const R&)) {
    Value v = Value::array();
    for (const R& r : removals) v.push(row(r));
    return v;
}

// The region's fields; its sequence inline in an operation, by index in
// the undo window, which shares one sequence between regions and steps.
Value midi_region_fields(const MidiRegion& r);

Value midi_region_value(const MidiRegion& r);

Value bus_value(const MonitorBus& b);

Value settings_value(const SessionSettings& s);

Value marker_value(const Marker& m);

Value plugin_value(const PluginRef& p);

Value params_value(const std::vector<ParamValue>& params);

// Every key is always present in an operation, so "no state" is null.
Value state_value(const std::optional<StateRef>& s);

Value insert_value(const Insert& in);

Value insert_mix_value(const InsertMix& m);

Value send_value(const Send& sd);

Value send_mix_value(const SendMix& m);

Value bus_mix_value(const BusMix& m);

Value vca_mix_value(const VcaMix& m);

// A VCA or a folder: the same three fields (ADR-0048).
Value named_set_value(Id id, const std::string& name, const std::vector<Id>& members);

// Without its lanes, which the window writes by table index.
Value aux_bus_value(const Bus& b);

Value group_value(const Group& g);

Value with_kind(const char* kind);

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

    // A signed distance: ShiftRegions' delta.
    bool offset(const Value& obj, const std::string& path, const char* key, Frames& out) {
        const Value& v = *obj.find(key);
        std::int64_t n = 0;
        if (!v.is_number() || !v.is_integer_lexeme() || !v.to_int64(n) || n < -kMaxInteger ||
            n > kMaxInteger) {
            return fail(at(path, key), "must be an integer within +-" + std::to_string(kMaxInteger));
        }
        out = n;
        return true;
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

// A palette index (ADR-0023, update of 2026-11-02); a region's may also be
// kTrackColour, its track's.
bool read_colour(Reader& rd, const Value& obj, const std::string& path, bool region,
                 std::uint8_t& out);

// A playlist's or AddPlaylist's "freeze" (ADR-0051): `obj` holds the key.
bool read_freeze(Reader& rd, const Value& obj, const std::string& path,
                 std::optional<PlaylistFreeze>& out);

bool read_fade(Reader& rd, const Value& obj, const std::string& path, const char* key, Fade& f);

bool read_gain_line(Reader& rd, const Value& v, const std::string& p,
                    std::vector<GainPoint>& line);

bool read_region_value(Reader& rd, const Value& v, const std::string& p, Region& r);

bool read_region(Reader& rd, const Value& obj, const std::string& path, const char* key,
                 Region& r);

bool read_crossfade(Reader& rd, const Value& v, const std::string& p, Crossfade& x);

bool read_source_value(Reader& rd, const Value& v, const std::string& p, Source& s);

bool read_source(Reader& rd, const Value& obj, const std::string& path, Source& s);

bool read_port(Reader& rd, const Value& obj, const std::string& path, const char* key,
               std::string& port, std::uint8_t& channel);

bool read_routing(Reader& rd, const Value& obj, const std::string& path, MidiRouting& r);

template <typename T>
bool read_rows(Reader& rd, const Value& obj, const std::string& path, const char* key,
               bool (*read)(const Value&, std::vector<T>&, detail::RowError&),
               std::vector<T>& out) {
    detail::RowError e;
    if (read(*obj.find(key), out, e)) return true;
    return rd.fail(Reader::at(path, key) + e.where, e.message);
}

bool read_data_ref(Reader& rd, const Value& obj, std::optional<MidiDataRef>& out);

bool read_midi_source_value(Reader& rd, const Value& v, const std::string& p, MidiSource& s);

bool read_midi_region_fields(Reader& rd, const Value& v, const std::string& p, MidiRegion& r);

bool read_midi_region_value(Reader& rd, const Value& v, const std::string& p, MidiRegion& r);

bool read_mix_value(Reader& rd, const Value& v, const std::string& p, TrackMix& m);

// Shape only; apply() and check_invariants() judge order and range.
bool read_points(Reader& rd, const Value& obj, const std::string& p,
                 std::vector<AutomationPoint>& out);

// "param", the keys lane_keys() asks of it and nothing else.
bool read_lane_target(Reader& rd, const Value& v, const std::string& p, AutomationParam& param,
                      Id& insert, Id& send, std::string& key);

bool read_lane(Reader& rd, const Value& e, const std::string& p, AutomationLane& lane);

bool read_mix(Reader& rd, const Value& obj, const std::string& path, TrackMix& m);

bool read_bus(Reader& rd, const Value& obj, MonitorBus& b);

bool read_settings(Reader& rd, const Value& obj, SessionSettings& s);

bool read_marker_value(Reader& rd, const Value& v, const std::string& p, Marker& m);

bool read_marker(Reader& rd, const Value& obj, Marker& m);

bool read_plugin(Reader& rd, const Value& obj, const std::string& path, PluginRef& p);

bool read_params(Reader& rd, const Value& obj, const std::string& path,
                 std::vector<ParamValue>& out);

bool read_state(Reader& rd, const Value& obj, const std::string& path,
                std::optional<StateRef>& out);

bool read_insert_value(Reader& rd, const Value& v, const std::string& p, Insert& in);

bool read_insert_mix_value(Reader& rd, const Value& v, const std::string& p, InsertMix& m);

bool read_send_value(Reader& rd, const Value& v, const std::string& p, Send& sd);

bool read_send_mix_value(Reader& rd, const Value& v, const std::string& p, SendMix& m);

bool read_bus_mix_value(Reader& rd, const Value& v, const std::string& p, BusMix& m);

bool read_vca_mix_value(Reader& rd, const Value& v, const std::string& p, VcaMix& m);

bool read_named_set(Reader& rd, const Value& v, const std::string& p, bool with_op, Id& id,
                    std::string& name, std::vector<Id>& members);

bool read_aux_bus_value(Reader& rd, const Value& v, const std::string& p, Bus& b);

bool read_group_value(Reader& rd, const Value& v, const std::string& p, bool with_op, Group& g);

// --- the families -----------------------------------------------------------

// Each family's file (op_codec_<family>.cpp). decode() sets `known` false,
// and leaves `out` alone, when `kind` is not one of the family's.
Value encode(const SessionOp& op);
Value encode(const TrackOp& op);
Value encode(const RegionOp& op);
Value encode(const ClipOp& op);
Value encode(const PlaylistOp& op);
Value encode(const InsertOp& op);
Value encode(const MixOp& op);
Value encode(const GroupOp& op);
Value encode(const MidiOp& op);
Value encode(const Compound& o);
bool decode(Reader& rd, const Value& v, const std::string& kind, SessionOp& out,
            bool& known);
bool decode(Reader& rd, const Value& v, const std::string& kind, TrackOp& out,
            bool& known);
bool decode(Reader& rd, const Value& v, const std::string& kind, RegionOp& out,
            bool& known);
bool decode(Reader& rd, const Value& v, const std::string& kind, ClipOp& out,
            bool& known);
bool decode(Reader& rd, const Value& v, const std::string& kind, PlaylistOp& out,
            bool& known);
bool decode(Reader& rd, const Value& v, const std::string& kind, InsertOp& out,
            bool& known);
bool decode(Reader& rd, const Value& v, const std::string& kind, MixOp& out,
            bool& known);
bool decode(Reader& rd, const Value& v, const std::string& kind, GroupOp& out,
            bool& known);
bool decode(Reader& rd, const Value& v, const std::string& kind, MidiOp& out,
            bool& known);

// --- the undo window ---------------------------------------------------------

// 2: tracks carry their inserts, and the mix the inserts' values.
// 3: each step carries its label (ADR-0009 update 2026-10-14), for "Undo
//    Paste" on a replica rebuilt from the window.
// 4: each track's mix carries solo_safe.
// 5: MIDI (ADR-0014): track kinds, MIDI playlists, the sequences their
//    regions share, MIDI routing in the mix, midi_sources.
// 6: a sequence whose file was missing at open carries its reference
//    (ADR-0015 O2), so a replica shows the same regions silent and saves
//    them back with it.
// 7: sequences are spines of chunks (ADR-0015 §1, §4): the chunks travel
//    apart, in parts, as a chunk table; the window holds the table's size,
//    a table of spine leaves listing chunks by table index, and sequences
//    listing leaves.
// 8: sends on each edit track, aux buses on each edit state, and the mix's
//    bus_mix and send_mix (ADR-0022).
// 9: each edit track and aux bus carries its output, each bus mix its solo
//    (ADR-0024).
// 10: each edit track carries its automation lanes, each track mix its
//    automation mode (ADR-0027).
// 11: a lane may be a plugin lane, with its insert and key (ADR-0027 §5,
//    update of 2026-11-01).
// 12: each edit track and region carries its colour (ADR-0023, update of
//    2026-11-02).
// 13: a lane may be a volume_trim lane, a track mix's automation mode
//    write_trim (ADR-0027 §8 slice 5).
// 14: an insert mix's state carries "files" (ADR-0029 slice 2).
// 15: lanes are one table, "lanes", that each edit track's, aux bus's and
//    edit state's "master_automation" index; a lane may be a mute, send
//    level or send pan lane, the last two with their send (ADR-0027 §9).
// 16: the session-model train (one window format for three features): an edit
//    state carries "groups" (ADR-0041), a region carries "gain_line"
//    (ADR-0042), each track mix carries track_punch and the transport
//    input_only, rolls_on and quick_punch (ADR-0043).
// 20 (clip groups and loops, ADR-0052; follows 16, 17 to 19 are other work's): a
//    region carries "clip_group" and "loop", and a region's sources may be
//    replaced by a sample edit (SetRegionSources).
// 17 (ADR-0048): the edit state also carries "vcas" and "folders", the mix
//    "vca_mix"; 18 and 19 are other work's.
constexpr std::int64_t kWindowFormat = 20;

}  // namespace sonora::model::codec
