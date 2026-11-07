// SPDX-License-Identifier: GPL-3.0-or-later
//
// The op codec (op_codec.h) for the family RegionOp (ADR-0059):
// what places, moves, cuts or records audio regions.
#include "op_codec_detail.h"

namespace sonora::model::codec {

namespace {

struct Encoder {
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
    Value operator()(const RecordLoopTake& o) const {
        Value v = with_kind("RecordLoopTake");
        v.set("track", id_value(o.track));
        v.set("active_playlist", id_value(o.active_playlist));
        Value& ps = v.set("passes", Value::array());
        for (const LoopTakePass& p : o.passes) {
            Value& e = ps.push(Value::object());
            e.set("playlist", id_value(p.playlist));
            e.set("name", Value::string(p.name));
            e.set("region", region_value(p.region));
        }
        v.set("chosen", integer(static_cast<std::int64_t>(o.chosen)));
        v.set("chosen_region", id_value(o.chosen_region));
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
        v.set("timebase", name_of(kTimebases, o.timebase));
        return v;
    }
    Value operator()(const TrimRegion& o) const {
        Value v = with_kind("TrimRegion");
        v.set("playlist", id_value(o.playlist));
        v.set("region", id_value(o.region));
        v.set("edge", name_of(kEdges, o.edge));
        v.set("to", integer(o.to));
        v.set("timebase", name_of(kTimebases, o.timebase));
        return v;
    }
    Value operator()(const SplitRegion& o) const {
        Value v = with_kind("SplitRegion");
        v.set("playlist", id_value(o.playlist));
        v.set("region", id_value(o.region));
        v.set("at", integer(o.at));
        v.set("new_right_id", id_value(o.new_right_id));
        v.set("timebase", name_of(kTimebases, o.timebase));
        return v;
    }
    Value operator()(const RemoveRegion& o) const {
        Value v = with_kind("RemoveRegion");
        v.set("playlist", id_value(o.playlist));
        v.set("region", id_value(o.region));
        return v;
    }
    Value operator()(const ShiftRegions& o) const {
        Value v = with_kind("ShiftRegions");
        v.set("playlist", id_value(o.playlist));
        v.set("from", integer(o.from));
        v.set("delta", integer(o.delta));
        return v;
    }
};

}  // namespace

Value encode(const RegionOp& op) { return std::visit(Encoder{}, op); }

bool decode(Reader& rd, const Value& v, const std::string& kind, RegionOp& out,
            bool& known) {
    known = true;
    if (kind == "PlaceRegion") {
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
    } else if (kind == "RecordLoopTake") {
        RecordLoopTake o;
        const Value* ps = nullptr;
        const Value* xs = nullptr;
        std::int64_t chosen = 0;
        if (!rd.keys(v, "", {"op", "track", "active_playlist", "passes", "chosen", "chosen_region",
                             "split_right_id", "crossfades"}) ||
            !rd.id(v, "", "track", o.track) ||
            !rd.id(v, "", "active_playlist", o.active_playlist) ||
            !rd.array(v, "", "passes", ps) ||
            !rd.integer(v, "", "chosen", static_cast<std::int64_t>(kMaxLoopTakePasses), chosen) ||
            !rd.id(v, "", "chosen_region", o.chosen_region) ||
            !rd.id(v, "", "split_right_id", o.split_right_id) ||
            !rd.array(v, "", "crossfades", xs)) {
            return false;
        }
        // Bounded before anything is built from it, as a compound is.
        if (ps->items().size() > kMaxLoopTakePasses) {
            return rd.fail("passes", "at most " + std::to_string(kMaxLoopTakePasses) +
                                         " passes; this one has " +
                                         std::to_string(ps->items().size()));
        }
        o.chosen = static_cast<std::size_t>(chosen);
        for (const Value& e : ps->items()) {
            LoopTakePass p;
            if (!rd.keys(e, "passes", {"playlist", "name", "region"}) ||
                !rd.id(e, "passes", "playlist", p.playlist) ||
                !rd.text(e, "passes", "name", p.name) ||
                !read_region(rd, e, "passes", "region", p.region)) {
                return false;
            }
            o.passes.push_back(std::move(p));
        }
        for (const Value& e : xs->items()) {
            Crossfade x;
            if (!read_crossfade(rd, e, "crossfades", x)) return false;
            o.crossfades.push_back(x);
        }
        out = std::move(o);
    } else if (kind == "MoveRegion") {
        MoveRegion o;
        if (!rd.keys(v, "", {"op", "playlist", "region", "position", "split_right_id",
                             "timebase"}) ||
            !rd.id(v, "", "playlist", o.playlist) || !rd.id(v, "", "region", o.region) ||
            !rd.frames(v, "", "position", o.position) ||
            !rd.id(v, "", "split_right_id", o.split_right_id) ||
            !rd.choice(v, "", "timebase", kTimebases, o.timebase)) {
            return false;
        }
        out = o;
    } else if (kind == "TrimRegion") {
        TrimRegion o;
        if (!rd.keys(v, "", {"op", "playlist", "region", "edge", "to", "timebase"}) ||
            !rd.id(v, "", "playlist", o.playlist) || !rd.id(v, "", "region", o.region) ||
            !rd.choice(v, "", "edge", kEdges, o.edge) || !rd.frames(v, "", "to", o.to) ||
            !rd.choice(v, "", "timebase", kTimebases, o.timebase)) {
            return false;
        }
        out = o;
    } else if (kind == "SplitRegion") {
        SplitRegion o;
        if (!rd.keys(v, "", {"op", "playlist", "region", "at", "new_right_id", "timebase"}) ||
            !rd.id(v, "", "playlist", o.playlist) || !rd.id(v, "", "region", o.region) ||
            !rd.frames(v, "", "at", o.at) || !rd.id(v, "", "new_right_id", o.new_right_id) ||
            !rd.choice(v, "", "timebase", kTimebases, o.timebase)) {
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
    } else if (kind == "ShiftRegions") {
        ShiftRegions o;
        if (!rd.keys(v, "", {"op", "playlist", "from", "delta"}) ||
            !rd.id(v, "", "playlist", o.playlist) || !rd.frames(v, "", "from", o.from) ||
            !rd.offset(v, "", "delta", o.delta)) {
            return false;
        }
        out = o;
    } else {
        known = false;
        return false;
    }
    return true;
}

}  // namespace sonora::model::codec
