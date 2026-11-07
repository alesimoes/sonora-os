// SPDX-License-Identifier: GPL-3.0-or-later
//
// The op codec (op_codec.h) for the family SessionOp (ADR-0059):
// sources, the monitor bus, the session settings and markers.
#include "op_codec_detail.h"

namespace sonora::model::codec {

namespace {

struct Encoder {
    Value operator()(const AddSource& o) const {
        Value v = with_kind("AddSource");
        v.set("source", source_value(o.source));
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
};

}  // namespace

Value encode(const SessionOp& op) { return std::visit(Encoder{}, op); }

bool decode(Reader& rd, const Value& v, const std::string& kind, SessionOp& out,
            bool& known) {
    known = true;
    if (kind == "AddSource") {
        AddSource o;
        if (!rd.keys(v, "", {"op", "source"}) || !read_source(rd, v, "", o.source)) return false;
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
    } else {
        known = false;
        return false;
    }
    return true;
}

}  // namespace sonora::model::codec
