// SPDX-License-Identifier: GPL-3.0-or-later
//
// The op codec (op_codec.h) for the family MidiOp (ADR-0059):
// MIDI sources, regions, edits and routing.
#include "op_codec_detail.h"

namespace sonora::model::codec {

namespace {

struct Encoder {
    Value operator()(const AddMidiSource& o) const {
        Value v = with_kind("AddMidiSource");
        v.set("source", midi_source_value(o.source));
        return v;
    }
    Value operator()(const PlaceMidiRegion& o) const {
        Value v = with_kind("PlaceMidiRegion");
        v.set("playlist", id_value(o.playlist));
        v.set("region", midi_region_value(o.region));
        v.set("split_right_id", id_value(o.split_right_id));
        if (o.data_from) {
            Value& ref = v.set("data_from", Value::object());
            ref.set("playlist", id_value(o.data_from->playlist));
            ref.set("region", id_value(o.data_from->region));
        } else {
            v.set("data_from", Value::null());
        }
        return v;
    }
    Value operator()(const EditMidi& o) const {
        Value v = with_kind("EditMidi");
        v.set("playlist", id_value(o.playlist));
        v.set("region", id_value(o.region));
        v.set("remove_notes", removals_value(o.remove_notes, detail::note_removal_row));
        v.set("add_notes", notes_value(o.add_notes));
        v.set("remove_events", removals_value(o.remove_events, detail::event_removal_row));
        v.set("add_events", events_value(o.add_events));
        v.set("remove_sysex", removals_value(o.remove_sysex, detail::sysex_removal_row));
        v.set("add_sysex", sysex_value(o.add_sysex));
        return v;
    }
    Value operator()(const SetMidiRouting& o) const {
        Value v = with_kind("SetMidiRouting");
        v.set("track", id_value(o.track));
        v.set("midi_input", port_value(o.routing.in_port, o.routing.in_channel));
        v.set("midi_output", port_value(o.routing.out_port, o.routing.out_channel));
        return v;
    }
};

}  // namespace

Value encode(const MidiOp& op) { return std::visit(Encoder{}, op); }

bool decode(Reader& rd, const Value& v, const std::string& kind, MidiOp& out,
            bool& known) {
    known = true;
    if (kind == "AddMidiSource") {
        AddMidiSource o;
        if (!rd.keys(v, "", {"op", "source"}) ||
            !read_midi_source_value(rd, *v.find("source"), "source", o.source)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "PlaceMidiRegion") {
        PlaceMidiRegion o;
        if (!rd.keys(v, "", {"op", "playlist", "region", "split_right_id", "data_from"}) ||
            !rd.id(v, "", "playlist", o.playlist) ||
            !read_midi_region_value(rd, *v.find("region"), "region", o.region) ||
            !rd.id(v, "", "split_right_id", o.split_right_id) ||
            !read_data_ref(rd, v, o.data_from)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "EditMidi") {
        EditMidi o;
        if (!rd.keys(v, "", {"op", "playlist", "region", "remove_notes", "add_notes",
                             "remove_events", "add_events", "remove_sysex", "add_sysex"}) ||
            !rd.id(v, "", "playlist", o.playlist) || !rd.id(v, "", "region", o.region) ||
            !read_rows(rd, v, "", "remove_notes", detail::read_note_removal_rows,
                       o.remove_notes) ||
            !read_rows(rd, v, "", "add_notes", detail::read_note_rows, o.add_notes) ||
            !read_rows(rd, v, "", "remove_events", detail::read_event_removal_rows,
                       o.remove_events) ||
            !read_rows(rd, v, "", "add_events", detail::read_event_rows, o.add_events) ||
            !read_rows(rd, v, "", "remove_sysex", detail::read_sysex_removal_rows,
                       o.remove_sysex) ||
            !read_rows(rd, v, "", "add_sysex", detail::read_sysex_rows, o.add_sysex)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "SetMidiRouting") {
        SetMidiRouting o;
        if (!rd.keys(v, "", {"op", "track", "midi_input", "midi_output"}) ||
            !rd.id(v, "", "track", o.track) || !read_routing(rd, v, "", o.routing)) {
            return false;
        }
        out = std::move(o);
    } else {
        known = false;
        return false;
    }
    return true;
}

}  // namespace sonora::model::codec
