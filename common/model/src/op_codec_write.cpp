// SPDX-License-Identifier: GPL-3.0-or-later
//
// The values every operation and the undo window are written with (op_codec_detail.h).
#include "op_codec_detail.h"

namespace sonora::model::codec {

Value integer(std::int64_t n) { return Value::integer(n); }
Value id_value(Id id) { return Value::integer(static_cast<std::int64_t>(id)); }

Value freeze_value(const PlaylistFreeze& f) {
    Value v = Value::object();
    v.set("from", id_value(f.from));
    Value& ins = v.set("inserts", Value::array());
    for (const Id i : f.inserts) ins.push(id_value(i));
    return v;
}

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

Value gain_line_value(const std::vector<GainPoint>& line) {
    Value a = Value::array();
    for (const GainPoint& p : line) {
        Value& e = a.push(Value::object());
        e.set("at", integer(p.at));
        e.set("db", Value::number_float(p.db));
    }
    return a;
}

Value region_value(const Region& r) {
    Value v = Value::object();
    v.set("id", id_value(r.id));
    v.set("name", Value::string(r.name));
    v.set("colour", integer(r.colour));
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
    v.set("gain_line", gain_line_value(r.gain_line));
    v.set("mute", Value::boolean(r.mute));
    v.set("fade_in", fade_value(r.fade_in));
    v.set("fade_out", fade_value(r.fade_out));
    v.set("clip_group", id_value(r.clip_group));
    v.set("loop", integer(r.loop));
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

Value port_value(const std::string& port, std::uint8_t channel) {
    Value v = Value::object();
    v.set("port", Value::string(port));
    v.set("channel", integer(channel));
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
    v.set("solo_safe", Value::boolean(m.solo_safe));
    v.set("midi_input", port_value(m.midi.in_port, m.midi.in_channel));
    v.set("midi_output", port_value(m.midi.out_port, m.midi.out_channel));
    v.set("automation", name_of(kAutomationModes, m.automation));
    v.set("track_punch", Value::boolean(m.track_punch));
    return v;
}

Value points_value(const std::vector<AutomationPoint>& points) {
    Value v = Value::array();
    for (const AutomationPoint& p : points) {
        Value e = Value::array();
        e.push(integer(p.at));
        e.push(Value::number_float(p.value));
        v.push(std::move(e));
    }
    return v;
}

void set_lane_target(Value& v, AutomationParam param, Id insert, Id send,
                     const std::string& key) {
    v.set("param", name_of(kAutomationParams, param));
    if (param == AutomationParam::Plugin) {
        v.set("insert", id_value(insert));
        v.set("key", Value::string(key));
    }
    if (param == AutomationParam::SendLevel || param == AutomationParam::SendPan) {
        v.set("send", id_value(send));
    }
}

LaneKeys lane_keys(const Value& v) {
    const Value* p = v.is_object() ? v.find("param") : nullptr;
    if (p == nullptr || !p->is_string()) return LaneKeys::Plain;
    if (p->text() == "plugin") return LaneKeys::Plugin;
    if (p->text() == "send_level" || p->text() == "send_pan") return LaneKeys::Send;
    return LaneKeys::Plain;
}

Value lane_value(const AutomationLane& l) {
    Value v = Value::object();
    set_lane_target(v, l.param, l.insert, l.send, l.key);
    v.set("points", points_value(l.points));
    return v;
}

Value midi_source_value(const MidiSource& s) {
    Value v = Value::object();
    v.set("id", id_value(s.id));
    v.set("path", Value::string(s.path));
    v.set("length", integer(s.length));
    v.set("events", integer(static_cast<std::int64_t>(s.events)));
    v.set("time_reference", integer(s.time_reference));
    v.set("kind", name_of(kKinds, s.kind));
    v.set("take", integer(s.take));
    v.set("recorded_on_track", id_value(s.recorded_on_track));
    v.set("latency_compensation", integer(s.latency_compensation));
    return v;
}

Value midi_region_fields(const MidiRegion& r) {
    Value v = Value::object();
    v.set("id", id_value(r.id));
    v.set("name", Value::string(r.name));
    v.set("colour", integer(r.colour));
    v.set("position_ticks", integer(r.position));
    v.set("length_ticks", integer(r.length));
    v.set("offset_ticks", integer(r.offset));
    v.set("mute", Value::boolean(r.mute));
    v.set("source", id_value(r.source));
    return v;
}

Value midi_region_value(const MidiRegion& r) {
    Value v = midi_region_fields(r);
    static const MidiSequence kEmpty;
    const MidiSequence& d = r.data ? *r.data : kEmpty;
    v.set("notes", notes_value(d.notes()));
    v.set("events", events_value(d.events()));
    v.set("sysex", sysex_value(d.sysex()));
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
    tr.set("count_off_bars", integer(s.transport.count_off_bars));
    tr.set("loop_playback", Value::boolean(s.transport.loop_playback));
    tr.set("loop_record", Value::boolean(s.transport.loop_record));
    tr.set("input_only", Value::boolean(s.transport.input_only));
    tr.set("rolls_on", Value::boolean(s.transport.rolls_on));
    tr.set("quick_punch", Value::boolean(s.transport.quick_punch));
    v.set("session_id", Value::string(s.session_id));
    Value& left = v.set("left_takes", Value::array());
    for (const std::string& path : s.left_takes) left.push(Value::string(path));
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

Value state_value(const std::optional<StateRef>& s) {
    if (!s) return Value::null();
    Value v = Value::object();
    v.set("file", Value::string(s->file));
    v.set("sha256", Value::string(s->sha256));
    v.set("bytes", integer(static_cast<std::int64_t>(s->bytes)));
    Value& fs = v.set("files", Value::array());
    for (const PluginFileRef& f : s->files) {
        Value& e = fs.push(Value::object());
        e.set("path", Value::string(f.path));
        e.set("sha256", Value::string(f.sha256));
        e.set("bytes", integer(static_cast<std::int64_t>(f.bytes)));
    }
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

Value send_value(const Send& sd) {
    Value v = Value::object();
    v.set("id", id_value(sd.id));
    v.set("slot", integer(sd.slot));
    v.set("bus", id_value(sd.bus));
    return v;
}

Value send_mix_value(const SendMix& m) {
    Value v = Value::object();
    v.set("send", id_value(m.send));
    v.set("level_db", Value::number_float(m.level_db));
    v.set("pan", Value::number_float(m.pan));
    v.set("follow_pan", Value::boolean(m.follow_pan));
    v.set("mute", Value::boolean(m.mute));
    v.set("pre", Value::boolean(m.pre));
    return v;
}

Value bus_mix_value(const BusMix& m) {
    Value v = Value::object();
    v.set("bus", id_value(m.bus));
    v.set("gain_db", Value::number_float(m.gain_db));
    v.set("pan", Value::number_float(m.pan));
    v.set("mute", Value::boolean(m.mute));
    v.set("solo", Value::boolean(m.solo));
    return v;
}

Value vca_mix_value(const VcaMix& m) {
    Value v = Value::object();
    v.set("vca", id_value(m.vca));
    v.set("gain_db", Value::number_float(m.gain_db));
    v.set("mute", Value::boolean(m.mute));
    return v;
}

Value named_set_value(Id id, const std::string& name, const std::vector<Id>& members) {
    Value v = Value::object();
    v.set("id", id_value(id));
    v.set("name", Value::string(name));
    Value& list = v.set("members", Value::array());
    for (const Id m : members) list.push(id_value(m));
    return v;
}

Value aux_bus_value(const Bus& b) {
    Value v = Value::object();
    v.set("id", id_value(b.id));
    v.set("name", Value::string(b.name));
    Value& sends = v.set("sends", Value::array());
    for (const Send& sd : b.sends) sends.push(send_value(sd));
    v.set("output", id_value(b.output));
    // Always present, as every key of the window is; kNoHardwareOut for an
    // ordinary bus (ADR-0060).
    v.set("hardware", integer(b.hardware));
    return v;
}

Value group_value(const Group& g) {
    Value v = Value::object();
    v.set("id", id_value(g.id));
    v.set("name", Value::string(g.name));
    Value& members = v.set("members", Value::array());
    for (const Id m : g.members) members.push(id_value(m));
    v.set("attrs", integer(g.attrs));
    v.set("relative", Value::boolean(g.relative));
    v.set("active", Value::boolean(g.active));
    return v;
}

Value with_kind(const char* kind) {
    Value v = Value::object();
    v.set("op", Value::string(kind));
    return v;
}

}  // namespace sonora::model::codec
