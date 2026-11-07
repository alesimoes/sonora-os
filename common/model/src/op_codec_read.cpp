// SPDX-License-Identifier: GPL-3.0-or-later
//
// The values every operation and the undo window are read with (op_codec_detail.h).
#include "op_codec_detail.h"

namespace sonora::model::codec {

bool read_colour(Reader& rd, const Value& obj, const std::string& path, bool region,
                 std::uint8_t& out) {
    std::uint8_t c = 0;
    if (!rd.small(obj, path, "colour", c)) return false;
    if (region ? !detail::region_colour_ok(c) : c >= kColours) {
        return rd.fail(Reader::at(path, "colour"), region ? detail::region_colour_problem()
                                                          : detail::track_colour_problem());
    }
    out = c;
    return true;
}

bool read_freeze(Reader& rd, const Value& obj, const std::string& path,
                 std::optional<PlaylistFreeze>& out) {
    const Value& v = *obj.find("freeze");
    const std::string p = Reader::at(path, "freeze");
    PlaylistFreeze f;
    const Value* ins = nullptr;
    if (!rd.keys(v, p, {"from", "inserts"}) || !rd.id(v, p, "from", f.from) ||
        !rd.array(v, p, "inserts", ins)) {
        return false;
    }
    if (ins->items().size() > kInsertSlots) return rd.fail(Reader::at(p, "inserts"), "holds too many inserts");
    for (const Value& e : ins->items()) {
        std::int64_t n = 0;
        if (!e.is_integer_lexeme() || !e.to_int64(n) || n < 0 || n > kMaxInteger) {
            return rd.fail(Reader::at(p, "inserts"), "must hold insert ids");
        }
        f.inserts.push_back(static_cast<Id>(n));
    }
    out = std::move(f);
    return true;
}

bool read_fade(Reader& rd, const Value& obj, const std::string& path, const char* key, Fade& f) {
    const Value& v = *obj.find(key);
    const std::string p = Reader::at(path, key);
    return rd.keys(v, p, {"length", "shape"}) && rd.frames(v, p, "length", f.length) &&
           rd.choice(v, p, "shape", kShapes, f.shape);
}

bool read_gain_line(Reader& rd, const Value& v, const std::string& p,
                    std::vector<GainPoint>& line) {
    const Value* a = nullptr;
    if (!rd.array(v, p, "gain_line", a)) return false;
    if (a->items().size() > kMaxGainPoints) {
        return rd.fail("gain_line", "holds more than " + std::to_string(kMaxGainPoints) + " points");
    }
    const std::string gp = p + ".gain_line";
    for (const Value& e : a->items()) {
        if (!rd.keys(e, gp, {"at", "db"})) return false;
        GainPoint pt;
        if (!rd.frames(e, gp, "at", pt.at) || !rd.real(e, gp, "db", pt.db)) return false;
        line.push_back(pt);
    }
    return true;
}

bool read_region_value(Reader& rd, const Value& v, const std::string& p, Region& r) {
    if (!rd.keys(v, p, {"id", "name", "colour", "position", "length", "source",
                        "source_offset", "gain_db", "gain_line", "mute", "fade_in",
                        "fade_out", "clip_group", "loop"})) {
        return false;
    }
    if (!rd.id(v, p, "id", r.id) || !rd.text(v, p, "name", r.name) ||
        !read_colour(rd, v, p, true, r.colour) ||
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
           rd.real(v, p, "gain_db", r.gain_db) && read_gain_line(rd, v, p, r.gain_line) &&
           rd.flag(v, p, "mute", r.mute) &&
           read_fade(rd, v, p, "fade_in", r.fade_in) &&
           read_fade(rd, v, p, "fade_out", r.fade_out) && rd.id(v, p, "clip_group", r.clip_group) &&
           rd.small(v, p, "loop", r.loop);
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

bool read_port(Reader& rd, const Value& obj, const std::string& path, const char* key,
               std::string& port, std::uint8_t& channel) {
    const Value& v = *obj.find(key);
    const std::string p = Reader::at(path, key);
    return rd.keys(v, p, {"port", "channel"}) && rd.text(v, p, "port", port) &&
           rd.small(v, p, "channel", channel);
}

bool read_routing(Reader& rd, const Value& obj, const std::string& path, MidiRouting& r) {
    return read_port(rd, obj, path, "midi_input", r.in_port, r.in_channel) &&
           read_port(rd, obj, path, "midi_output", r.out_port, r.out_channel);
}

bool read_data_ref(Reader& rd, const Value& obj, std::optional<MidiDataRef>& out) {
    const Value& v = *obj.find("data_from");
    if (v.is_null()) {
        out.reset();
        return true;
    }
    MidiDataRef ref;
    if (!rd.keys(v, "data_from", {"playlist", "region"}) ||
        !rd.id(v, "data_from", "playlist", ref.playlist) ||
        !rd.id(v, "data_from", "region", ref.region)) {
        return false;
    }
    out = ref;
    return true;
}

bool read_midi_source_value(Reader& rd, const Value& v, const std::string& p, MidiSource& s) {
    std::int64_t events = 0;
    const bool ok =
        rd.keys(v, p, {"id", "path", "length", "events", "time_reference", "kind", "take",
                       "recorded_on_track", "latency_compensation"}) &&
        rd.id(v, p, "id", s.id) && rd.text(v, p, "path", s.path) &&
        rd.frames(v, p, "length", s.length) &&
        rd.integer(v, p, "events", kMaxInteger, events) &&
        rd.integer(v, p, "time_reference", kMaxInteger, s.time_reference) &&
        rd.choice(v, p, "kind", kKinds, s.kind) &&
        rd.integer(v, p, "take", kMaxInteger, s.take) &&
        rd.id(v, p, "recorded_on_track", s.recorded_on_track) &&
        rd.frames(v, p, "latency_compensation", s.latency_compensation);
    s.events = static_cast<std::uint64_t>(events);
    return ok;
}

bool read_midi_region_fields(Reader& rd, const Value& v, const std::string& p, MidiRegion& r) {
    return rd.id(v, p, "id", r.id) && rd.text(v, p, "name", r.name) &&
           read_colour(rd, v, p, true, r.colour) &&
           rd.frames(v, p, "position_ticks", r.position) &&
           rd.frames(v, p, "length_ticks", r.length) &&
           rd.frames(v, p, "offset_ticks", r.offset) && rd.flag(v, p, "mute", r.mute) &&
           rd.id(v, p, "source", r.source);
}

bool read_midi_region_value(Reader& rd, const Value& v, const std::string& p, MidiRegion& r) {
    if (!rd.keys(v, p, {"id", "name", "colour", "position_ticks", "length_ticks",
                        "offset_ticks", "mute", "source", "notes", "events", "sysex"}) ||
        !read_midi_region_fields(rd, v, p, r)) {
        return false;
    }
    MidiLists lists;
    if (!read_rows(rd, v, p, "notes", detail::read_note_rows, lists.notes) ||
        !read_rows(rd, v, p, "events", detail::read_event_rows, lists.events) ||
        !read_rows(rd, v, p, "sysex", detail::read_sysex_rows, lists.sysex)) {
        return false;
    }
    // As lists, before they are chunked, which would hide an order broken
    // across chunks; apply() checks the rest, as for any region.
    std::string where;
    const std::string why = detail::sequence_problem(lists, where);
    if (!why.empty()) return rd.fail(p.empty() ? where : p + "." + where, why);
    r.data = std::make_shared<const MidiSequence>(std::move(lists));
    return true;
}

bool read_mix_value(Reader& rd, const Value& v, const std::string& p, TrackMix& m) {
    if (!rd.keys(v, p,
                 {"track", "input", "monitor", "gain_db", "pan", "mute", "solo", "solo_safe",
                  "midi_input", "midi_output", "automation", "track_punch"}) ||
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
           rd.flag(v, p, "mute", m.mute) && rd.flag(v, p, "solo", m.solo) &&
           rd.flag(v, p, "solo_safe", m.solo_safe) && read_routing(rd, v, p, m.midi) &&
           rd.choice(v, p, "automation", kAutomationModes, m.automation) &&
           rd.flag(v, p, "track_punch", m.track_punch);
}

bool read_points(Reader& rd, const Value& obj, const std::string& p,
                 std::vector<AutomationPoint>& out) {
    const Value* list = nullptr;
    if (!rd.array(obj, p, "points", list)) return false;
    if (list->items().size() > kMaxAutomationPoints) {
        return rd.fail(Reader::at(p, "points"),
                       "a lane holds at most " + std::to_string(kMaxAutomationPoints) + " points");
    }
    out.reserve(list->items().size());
    for (const Value& e : list->items()) {
        std::int64_t at = 0;
        float value = 0.0f;
        if (!e.is_array() || e.items().size() != 2 || !e.items()[0].is_integer_lexeme() ||
            !e.items()[0].to_int64(at) || at < 0 || at > kMaxInteger ||
            !e.items()[1].is_number() || !e.items()[1].to_float(value)) {
            return rd.fail(Reader::at(p, "points"), "must hold [frame, value] pairs");
        }
        out.push_back(AutomationPoint{at, value});
    }
    return true;
}

bool read_lane_target(Reader& rd, const Value& v, const std::string& p, AutomationParam& param,
                      Id& insert, Id& send, std::string& key) {
    const LaneKeys k = lane_keys(v);
    return rd.choice(v, p, "param", kAutomationParams, param) &&
           (k != LaneKeys::Plugin ||
            (rd.id(v, p, "insert", insert) && rd.text(v, p, "key", key))) &&
           (k != LaneKeys::Send || rd.id(v, p, "send", send));
}

bool read_lane(Reader& rd, const Value& e, const std::string& p, AutomationLane& lane) {
    const LaneKeys k = lane_keys(e);
    const bool keys = k == LaneKeys::Plugin ? rd.keys(e, p, {"param", "insert", "key", "points"})
                      : k == LaneKeys::Send ? rd.keys(e, p, {"param", "send", "points"})
                                            : rd.keys(e, p, {"param", "points"});
    return keys && read_lane_target(rd, e, p, lane.param, lane.insert, lane.send, lane.key) &&
           read_points(rd, e, p, lane.points);
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
                        "transport", "session_id", "left_takes"})) {
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
    std::int64_t bars = 0;
    if (!rd.keys(tr, trp,
                 {"loop", "punch", "pre_roll", "post_roll", "punch_crossfade", "count_off_bars",
                  "loop_playback", "loop_record", "input_only", "rolls_on", "quick_punch"}) ||
        !rd.range(tr, trp, "loop", kMaxInteger, loop) ||
        !rd.range(tr, trp, "punch", kMaxInteger, punch) ||
        !rd.frames(tr, trp, "pre_roll", s.transport.pre_roll) ||
        !rd.frames(tr, trp, "post_roll", s.transport.post_roll) ||
        !rd.frames(tr, trp, "punch_crossfade", s.transport.punch_crossfade) ||
        !rd.integer(tr, trp, "count_off_bars", kMaxCountOffBars, bars) ||
        !rd.flag(tr, trp, "loop_playback", s.transport.loop_playback) ||
        !rd.flag(tr, trp, "loop_record", s.transport.loop_record) ||
        !rd.flag(tr, trp, "input_only", s.transport.input_only) ||
        !rd.flag(tr, trp, "rolls_on", s.transport.rolls_on) ||
        !rd.flag(tr, trp, "quick_punch", s.transport.quick_punch)) {
        return false;
    }
    s.transport.count_off_bars = static_cast<std::uint32_t>(bars);
    s.transport.loop = loop;
    s.transport.punch = punch;
    const Value* left = nullptr;
    if (!rd.text(v, p, "session_id", s.session_id) || !rd.array(v, p, "left_takes", left)) {
        return false;
    }
    if (left->items().size() > kMaxLeftTakes) {
        return rd.fail(p + ".left_takes", "holds more than " + std::to_string(kMaxLeftTakes) +
                                              " files");
    }
    s.left_takes.clear();
    for (const Value& e : left->items()) {
        if (!e.is_string()) return rd.fail(p + ".left_takes", "must hold strings");
        s.left_takes.push_back(e.text());
    }
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
    const Value* files = nullptr;
    if (!rd.keys(v, sp, {"file", "sha256", "bytes", "files"}) || !rd.text(v, sp, "file", s.file) ||
        !rd.text(v, sp, "sha256", s.sha256) ||
        !rd.integer(v, sp, "bytes", kMaxInteger, bytes) || !rd.array(v, sp, "files", files)) {
        return false;
    }
    s.bytes = static_cast<std::uint64_t>(bytes);
    const std::string fp = Reader::at(sp, "files");
    if (files->items().size() > kMaxPluginFiles) return rd.fail(fp, "too many files");
    for (const Value& e : files->items()) {
        PluginFileRef f;
        std::int64_t fb = 0;
        if (!rd.keys(e, fp, {"path", "sha256", "bytes"}) || !rd.text(e, fp, "path", f.path) ||
            !rd.text(e, fp, "sha256", f.sha256) || !rd.integer(e, fp, "bytes", kMaxInteger, fb)) {
            return false;
        }
        f.bytes = static_cast<std::uint64_t>(fb);
        s.files.push_back(std::move(f));
    }
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

bool read_send_value(Reader& rd, const Value& v, const std::string& p, Send& sd) {
    return rd.keys(v, p, {"id", "slot", "bus"}) && rd.id(v, p, "id", sd.id) &&
           rd.small(v, p, "slot", sd.slot) && rd.id(v, p, "bus", sd.bus);
}

bool read_send_mix_value(Reader& rd, const Value& v, const std::string& p, SendMix& m) {
    return rd.keys(v, p, {"send", "level_db", "pan", "follow_pan", "mute", "pre"}) &&
           rd.id(v, p, "send", m.send) && rd.real(v, p, "level_db", m.level_db) &&
           rd.real(v, p, "pan", m.pan) && rd.flag(v, p, "follow_pan", m.follow_pan) &&
           rd.flag(v, p, "mute", m.mute) && rd.flag(v, p, "pre", m.pre);
}

bool read_bus_mix_value(Reader& rd, const Value& v, const std::string& p, BusMix& m) {
    return rd.keys(v, p, {"bus", "gain_db", "pan", "mute", "solo"}) &&
           rd.id(v, p, "bus", m.bus) && rd.real(v, p, "gain_db", m.gain_db) &&
           rd.real(v, p, "pan", m.pan) && rd.flag(v, p, "mute", m.mute) &&
           rd.flag(v, p, "solo", m.solo);
}

bool read_vca_mix_value(Reader& rd, const Value& v, const std::string& p, VcaMix& m) {
    return rd.keys(v, p, {"vca", "gain_db", "mute"}) && rd.id(v, p, "vca", m.vca) &&
           rd.real(v, p, "gain_db", m.gain_db) && rd.flag(v, p, "mute", m.mute);
}

bool read_named_set(Reader& rd, const Value& v, const std::string& p, bool with_op, Id& id,
                    std::string& name, std::vector<Id>& members) {
    const Value* list = nullptr;
    const bool keys = with_op ? rd.keys(v, p, {"op", "id", "name", "members"})
                              : rd.keys(v, p, {"id", "name", "members"});
    if (!keys || !rd.id(v, p, "id", id) || !rd.text(v, p, "name", name) ||
        !rd.array(v, p, "members", list)) {
        return false;
    }
    for (const Value& e : list->items()) {
        std::int64_t n = 0;
        if (!e.is_integer_lexeme() || !e.to_int64(n) || n < 1 || n > kMaxInteger) {
            return rd.fail(Reader::at(p, "members"), "must hold track ids");
        }
        members.push_back(static_cast<Id>(n));
    }
    return true;
}

bool read_aux_bus_value(Reader& rd, const Value& v, const std::string& p, Bus& b) {
    const Value* sends = nullptr;
    // Its "automation" is table indices, which read_edits resolves.
    if (!rd.keys(v, p, {"id", "name", "sends", "output", "automation", "hardware"}) ||
        !rd.id(v, p, "id", b.id) ||
        !rd.text(v, p, "name", b.name) || !rd.array(v, p, "sends", sends) ||
        !rd.id(v, p, "output", b.output)) {
        return false;
    }
    for (const Value& e : sends->items()) {
        Send sd;
        if (!read_send_value(rd, e, Reader::at(p, "sends"), sd)) return false;
        b.sends.push_back(sd);
    }
    std::uint16_t first = kNoHardwareOut;
    if (!rd.small(v, p, "hardware", first)) return false;
    if (first != kNoHardwareOut && (first % 2 != 0 || first + 1 >= kMaxOutputChannels)) {
        return rd.fail(Reader::at(p, "hardware"), "is not the first of a pair of outputs");
    }
    b.hardware = first;
    return true;
}

bool read_group_value(Reader& rd, const Value& v, const std::string& p, bool with_op, Group& g) {
    const Value* members = nullptr;
    std::int64_t attrs = 0;
    const bool keys = with_op ? rd.keys(v, p, {"op", "id", "name", "members", "attrs", "relative",
                                              "active"})
                              : rd.keys(v, p, {"id", "name", "members", "attrs", "relative",
                                              "active"});
    if (!keys || !rd.id(v, p, "id", g.id) || !rd.text(v, p, "name", g.name) ||
        !rd.array(v, p, "members", members) || !rd.integer(v, p, "attrs", 255, attrs) ||
        !rd.flag(v, p, "relative", g.relative) || !rd.flag(v, p, "active", g.active)) {
        return false;
    }
    g.attrs = static_cast<std::uint8_t>(attrs);
    for (const Value& e : members->items()) {
        std::int64_t n = 0;
        if (!e.is_integer_lexeme() || !e.to_int64(n) || n < 0 || n > kMaxInteger) {
            return rd.fail(Reader::at(p, "members"), "must hold track ids");
        }
        g.members.push_back(static_cast<Id>(n));
    }
    return true;
}

}  // namespace sonora::model::codec
