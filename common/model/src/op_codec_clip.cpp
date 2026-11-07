// SPDX-License-Identifier: GPL-3.0-or-later
//
// The op codec (op_codec.h) for the family ClipOp (ADR-0059):
// a region's own attributes and the fades between regions.
#include "op_codec_detail.h"

namespace sonora::model::codec {

namespace {

struct Encoder {
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
    Value operator()(const SetRegionName& o) const {
        Value v = with_kind("SetRegionName");
        v.set("playlist", id_value(o.playlist));
        v.set("region", id_value(o.region));
        v.set("name", Value::string(o.name));
        return v;
    }
    Value operator()(const SetRegionColour& o) const {
        Value v = with_kind("SetRegionColour");
        v.set("playlist", id_value(o.playlist));
        v.set("region", id_value(o.region));
        v.set("colour", integer(o.colour));
        return v;
    }
    Value operator()(const SetRegionGain& o) const {
        Value v = with_kind("SetRegionGain");
        v.set("playlist", id_value(o.playlist));
        v.set("region", id_value(o.region));
        v.set("gain_db", Value::number_float(o.gain_db));
        v.set("gain_line", gain_line_value(o.gain_line));
        return v;
    }
    Value operator()(const SetClipGroup& o) const {
        Value v = with_kind("SetClipGroup");
        v.set("playlist", id_value(o.playlist));
        v.set("group", id_value(o.group));
        Value& list = v.set("members", Value::array());
        for (const ClipGroupMember& m : o.members) {
            Value& e = list.push(Value::object());
            e.set("region", id_value(m.region));
            e.set("loop", integer(m.loop));
        }
        return v;
    }
    Value operator()(const SetRegionSources& o) const {
        Value v = with_kind("SetRegionSources");
        v.set("playlist", id_value(o.playlist));
        v.set("region", id_value(o.region));
        Value& src = v.set("source", Value::array());
        for (const SourceRef& c : o.channels) {
            Value& e = src.push(Value::object());
            e.set("id", id_value(c.source));
            e.set("channel", integer(c.channel));
        }
        v.set("source_offset", integer(o.source_offset));
        v.set("gain_line", gain_line_value(o.gain_line));
        return v;
    }
};

}  // namespace

Value encode(const ClipOp& op) { return std::visit(Encoder{}, op); }

bool decode(Reader& rd, const Value& v, const std::string& kind, ClipOp& out,
            bool& known) {
    known = true;
    if (kind == "SetFade") {
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
    } else if (kind == "SetRegionName") {
        SetRegionName o;
        if (!rd.keys(v, "", {"op", "playlist", "region", "name"}) ||
            !rd.id(v, "", "playlist", o.playlist) || !rd.id(v, "", "region", o.region) ||
            !rd.text(v, "", "name", o.name)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "SetRegionColour") {
        SetRegionColour o;
        if (!rd.keys(v, "", {"op", "playlist", "region", "colour"}) ||
            !rd.id(v, "", "playlist", o.playlist) || !rd.id(v, "", "region", o.region) ||
            !read_colour(rd, v, "", true, o.colour)) {
            return false;
        }
        out = o;
    } else if (kind == "SetRegionGain") {
        SetRegionGain o;
        if (!rd.keys(v, "", {"op", "playlist", "region", "gain_db", "gain_line"}) ||
            !rd.id(v, "", "playlist", o.playlist) || !rd.id(v, "", "region", o.region) ||
            !rd.real(v, "", "gain_db", o.gain_db) || !read_gain_line(rd, v, "", o.gain_line)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "SetClipGroup") {
        SetClipGroup o;
        const Value* members = nullptr;
        if (!rd.keys(v, "", {"op", "playlist", "group", "members"}) ||
            !rd.id(v, "", "playlist", o.playlist) || !rd.id(v, "", "group", o.group) ||
            !rd.array(v, "", "members", members)) {
            return false;
        }
        if (members->items().size() > kMaxCompoundMembers) {
            return rd.fail("members", "holds more than " + std::to_string(kMaxCompoundMembers) +
                                          " regions");
        }
        for (const Value& e : members->items()) {
            ClipGroupMember m;
            if (!rd.keys(e, "members", {"region", "loop"}) ||
                !rd.id(e, "members", "region", m.region) ||
                !rd.small(e, "members", "loop", m.loop)) {
                return false;
            }
            o.members.push_back(m);
        }
        out = std::move(o);
    } else if (kind == "SetRegionSources") {
        SetRegionSources o;
        const Value* src = nullptr;
        if (!rd.keys(v, "", {"op", "playlist", "region", "source", "source_offset", "gain_line"}) ||
            !rd.id(v, "", "playlist", o.playlist) || !rd.id(v, "", "region", o.region) ||
            !rd.array(v, "", "source", src)) {
            return false;
        }
        for (const Value& e : src->items()) {
            if (!rd.keys(e, "source", {"id", "channel"})) return false;
            SourceRef ref;
            if (!rd.id(e, "source", "id", ref.source) ||
                !rd.small(e, "source", "channel", ref.channel)) {
                return false;
            }
            o.channels.push_back(ref);
        }
        if (!rd.frames(v, "", "source_offset", o.source_offset) ||
            !read_gain_line(rd, v, "", o.gain_line)) {
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
