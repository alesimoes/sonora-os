// SPDX-License-Identifier: GPL-3.0-or-later
//
// The op codec (op_codec.h) for the family InsertOp (ADR-0059):
// plug-in inserts and their values.
#include "op_codec_detail.h"

namespace sonora::model::codec {

namespace {

struct Encoder {
    Value operator()(const AddInsert& o) const {
        Value v = with_kind("AddInsert");
        v.set("track", id_value(o.track));
        v.set("insert", id_value(o.insert));
        v.set("slot", integer(o.slot));
        v.set("plugin", plugin_value(o.plugin));
        v.set("params", params_value(o.params));
        v.set("state", state_value(o.state));
        return v;
    }
    Value operator()(const RemoveInsert& o) const {
        Value v = with_kind("RemoveInsert");
        v.set("insert", id_value(o.insert));
        return v;
    }
    Value operator()(const MoveInsert& o) const {
        Value v = with_kind("MoveInsert");
        v.set("insert", id_value(o.insert));
        v.set("to_track", id_value(o.to_track));
        v.set("to_slot", integer(o.to_slot));
        return v;
    }
    Value operator()(const SetInsertActive& o) const {
        Value v = with_kind("SetInsertActive");
        v.set("insert", id_value(o.insert));
        v.set("active", Value::boolean(o.active));
        return v;
    }
    Value operator()(const ReplaceInsert& o) const {
        Value v = with_kind("ReplaceInsert");
        v.set("insert", id_value(o.insert));
        v.set("replacement", id_value(o.replacement));
        v.set("plugin", plugin_value(o.plugin));
        v.set("params", params_value(o.params));
        v.set("state", state_value(o.state));
        return v;
    }
    Value operator()(const SetInsertBypass& o) const {
        Value v = with_kind("SetInsertBypass");
        v.set("insert", id_value(o.insert));
        v.set("bypass", Value::boolean(o.bypass));
        return v;
    }
    Value operator()(const SetInsertParams& o) const {
        Value v = with_kind("SetInsertParams");
        v.set("insert", id_value(o.insert));
        v.set("params", params_value(o.params));
        return v;
    }
    Value operator()(const SetInsertState& o) const {
        Value v = with_kind("SetInsertState");
        v.set("insert", id_value(o.insert));
        v.set("state", state_value(o.state));
        return v;
    }
};

}  // namespace

Value encode(const InsertOp& op) { return std::visit(Encoder{}, op); }

bool decode(Reader& rd, const Value& v, const std::string& kind, InsertOp& out,
            bool& known) {
    known = true;
    if (kind == "AddInsert") {
        AddInsert o;
        if (!rd.keys(v, "", {"op", "track", "insert", "slot", "plugin", "params", "state"}) ||
            !rd.id(v, "", "track", o.track) || !rd.id(v, "", "insert", o.insert) ||
            !rd.small(v, "", "slot", o.slot) || !read_plugin(rd, v, "", o.plugin) ||
            !read_params(rd, v, "", o.params) || !read_state(rd, v, "", o.state)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "RemoveInsert") {
        RemoveInsert o;
        if (!rd.keys(v, "", {"op", "insert"}) || !rd.id(v, "", "insert", o.insert)) return false;
        out = o;
    } else if (kind == "MoveInsert") {
        MoveInsert o;
        if (!rd.keys(v, "", {"op", "insert", "to_track", "to_slot"}) ||
            !rd.id(v, "", "insert", o.insert) || !rd.id(v, "", "to_track", o.to_track) ||
            !rd.small(v, "", "to_slot", o.to_slot)) {
            return false;
        }
        out = o;
    } else if (kind == "SetInsertActive") {
        SetInsertActive o;
        if (!rd.keys(v, "", {"op", "insert", "active"}) || !rd.id(v, "", "insert", o.insert) ||
            !rd.flag(v, "", "active", o.active)) {
            return false;
        }
        out = o;
    } else if (kind == "ReplaceInsert") {
        ReplaceInsert o;
        if (!rd.keys(v, "", {"op", "insert", "replacement", "plugin", "params", "state"}) ||
            !rd.id(v, "", "insert", o.insert) || !rd.id(v, "", "replacement", o.replacement) ||
            !read_plugin(rd, v, "", o.plugin) || !read_params(rd, v, "", o.params) ||
            !read_state(rd, v, "", o.state)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "SetInsertBypass") {
        SetInsertBypass o;
        if (!rd.keys(v, "", {"op", "insert", "bypass"}) || !rd.id(v, "", "insert", o.insert) ||
            !rd.flag(v, "", "bypass", o.bypass)) {
            return false;
        }
        out = o;
    } else if (kind == "SetInsertParams") {
        SetInsertParams o;
        if (!rd.keys(v, "", {"op", "insert", "params"}) || !rd.id(v, "", "insert", o.insert) ||
            !read_params(rd, v, "", o.params)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "SetInsertState") {
        SetInsertState o;
        if (!rd.keys(v, "", {"op", "insert", "state"}) || !rd.id(v, "", "insert", o.insert) ||
            !read_state(rd, v, "", o.state)) {
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
