// SPDX-License-Identifier: GPL-3.0-or-later
//
// The op codec (op_codec.h) for the family MixOp (ADR-0059):
// the mixer: faders, buses, sends, outputs and automation.
#include "op_codec_detail.h"

namespace sonora::model::codec {

namespace {

struct Encoder {
    Value operator()(const SetMix& o) const {
        Value v = with_kind("SetMix");
        v.set("mix", mix_value(o.mix));
        return v;
    }
    Value operator()(const AddBus& o) const {
        Value v = with_kind("AddBus");
        v.set("bus", id_value(o.bus));
        v.set("name", Value::string(o.name));
        v.set("mix", bus_mix_value(o.mix));
        return v;
    }
    Value operator()(const RemoveBus& o) const {
        Value v = with_kind("RemoveBus");
        v.set("bus", id_value(o.bus));
        return v;
    }
    Value operator()(const AddSend& o) const {
        Value v = with_kind("AddSend");
        v.set("strip", id_value(o.strip));
        v.set("send", id_value(o.send));
        v.set("slot", integer(o.slot));
        v.set("bus", id_value(o.bus));
        v.set("mix", send_mix_value(o.mix));
        return v;
    }
    Value operator()(const RemoveSend& o) const {
        Value v = with_kind("RemoveSend");
        v.set("send", id_value(o.send));
        return v;
    }
    Value operator()(const SetSendMix& o) const {
        Value v = with_kind("SetSendMix");
        v.set("mix", send_mix_value(o.mix));
        return v;
    }
    Value operator()(const SetBusMix& o) const {
        Value v = with_kind("SetBusMix");
        v.set("mix", bus_mix_value(o.mix));
        return v;
    }
    Value operator()(const SetOutput& o) const {
        Value v = with_kind("SetOutput");
        v.set("strip", id_value(o.strip));
        v.set("output", id_value(o.output));
        return v;
    }
    Value operator()(const SetBusPath& o) const {
        Value v = with_kind("SetBusPath");
        v.set("bus", id_value(o.bus));
        v.set("name", Value::string(o.name));
        v.set("first", integer(o.first));
        return v;
    }
    Value operator()(const EditAutomation& o) const {
        Value v = with_kind("EditAutomation");
        v.set("track", id_value(o.track));
        set_lane_target(v, o.param, o.insert, o.send, o.key);
        v.set("range", pair(o.from, o.to));
        v.set("points", points_value(o.points));
        return v;
    }
};

}  // namespace

Value encode(const MixOp& op) { return std::visit(Encoder{}, op); }

bool decode(Reader& rd, const Value& v, const std::string& kind, MixOp& out,
            bool& known) {
    known = true;
    if (kind == "SetMix") {
        SetMix o;
        if (!rd.keys(v, "", {"op", "mix"}) || !read_mix(rd, v, "", o.mix)) return false;
        out = std::move(o);
    } else if (kind == "AddBus") {
        AddBus o;
        if (!rd.keys(v, "", {"op", "bus", "name", "mix"}) || !rd.id(v, "", "bus", o.bus) ||
            !rd.text(v, "", "name", o.name) ||
            !read_bus_mix_value(rd, *v.find("mix"), "mix", o.mix)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "RemoveBus") {
        RemoveBus o;
        if (!rd.keys(v, "", {"op", "bus"}) || !rd.id(v, "", "bus", o.bus)) return false;
        out = o;
    } else if (kind == "AddSend") {
        AddSend o;
        if (!rd.keys(v, "", {"op", "strip", "send", "slot", "bus", "mix"}) ||
            !rd.id(v, "", "strip", o.strip) || !rd.id(v, "", "send", o.send) ||
            !rd.small(v, "", "slot", o.slot) || !rd.id(v, "", "bus", o.bus) ||
            !read_send_mix_value(rd, *v.find("mix"), "mix", o.mix)) {
            return false;
        }
        out = o;
    } else if (kind == "RemoveSend") {
        RemoveSend o;
        if (!rd.keys(v, "", {"op", "send"}) || !rd.id(v, "", "send", o.send)) return false;
        out = o;
    } else if (kind == "SetSendMix") {
        SetSendMix o;
        if (!rd.keys(v, "", {"op", "mix"}) ||
            !read_send_mix_value(rd, *v.find("mix"), "mix", o.mix)) {
            return false;
        }
        out = o;
    } else if (kind == "SetBusMix") {
        SetBusMix o;
        if (!rd.keys(v, "", {"op", "mix"}) ||
            !read_bus_mix_value(rd, *v.find("mix"), "mix", o.mix)) {
            return false;
        }
        out = o;
    } else if (kind == "SetOutput") {
        SetOutput o;
        if (!rd.keys(v, "", {"op", "strip", "output"}) || !rd.id(v, "", "strip", o.strip) ||
            !rd.id(v, "", "output", o.output)) {
            return false;
        }
        out = o;
    } else if (kind == "SetBusPath") {
        SetBusPath o;
        if (!rd.keys(v, "", {"op", "bus", "name", "first"}) || !rd.id(v, "", "bus", o.bus) ||
            !rd.text(v, "", "name", o.name) || !rd.small(v, "", "first", o.first)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "EditAutomation") {
        EditAutomation o;
        std::array<std::int64_t, 2> range{};
        const LaneKeys k = lane_keys(v);
        const bool keys =
            k == LaneKeys::Plugin
                ? rd.keys(v, "", {"op", "track", "param", "insert", "key", "range", "points"})
            : k == LaneKeys::Send
                ? rd.keys(v, "", {"op", "track", "param", "send", "range", "points"})
                : rd.keys(v, "", {"op", "track", "param", "range", "points"});
        if (!keys || !rd.id(v, "", "track", o.track) ||
            !read_lane_target(rd, v, "", o.param, o.insert, o.send, o.key) ||
            !rd.range(v, "", "range", kMaxInteger, range) || !read_points(rd, v, "", o.points)) {
            return false;
        }
        o.from = range[0];
        o.to = range[1];
        out = std::move(o);
    } else {
        known = false;
        return false;
    }
    return true;
}

}  // namespace sonora::model::codec
