// SPDX-License-Identifier: GPL-3.0-or-later
//
// The op codec (op_codec.h) for the family GroupOp (ADR-0059):
// mix and edit groups, VCAs and folders.
#include "op_codec_detail.h"

namespace sonora::model::codec {

namespace {

struct Encoder {
    Value operator()(const SetGroup& o) const {
        Group g;
        g.id = o.group;
        g.name = o.name;
        g.members = o.members;
        g.attrs = o.attrs;
        g.relative = o.relative;
        g.active = o.active;
        Value v = group_value(g);
        v.set("op", Value::string("SetGroup"));
        return v;
    }
    Value operator()(const RemoveGroup& o) const {
        Value v = with_kind("RemoveGroup");
        v.set("group", id_value(o.group));
        return v;
    }
    Value operator()(const SetVca& o) const {
        Value v = named_set_value(o.vca, o.name, o.members);
        v.set("op", Value::string("SetVca"));
        return v;
    }
    Value operator()(const RemoveVca& o) const {
        Value v = with_kind("RemoveVca");
        v.set("vca", id_value(o.vca));
        return v;
    }
    Value operator()(const SetVcaMix& o) const {
        Value v = with_kind("SetVcaMix");
        v.set("mix", vca_mix_value(o.mix));
        return v;
    }
    Value operator()(const SetFolder& o) const {
        Value v = named_set_value(o.folder, o.name, o.members);
        v.set("op", Value::string("SetFolder"));
        return v;
    }
    Value operator()(const RemoveFolder& o) const {
        Value v = with_kind("RemoveFolder");
        v.set("folder", id_value(o.folder));
        return v;
    }
};

}  // namespace

Value encode(const GroupOp& op) { return std::visit(Encoder{}, op); }

bool decode(Reader& rd, const Value& v, const std::string& kind, GroupOp& out,
            bool& known) {
    known = true;
    if (kind == "SetGroup") {
        Group g;
        if (!read_group_value(rd, v, "", true, g)) return false;
        SetGroup o;
        o.group = g.id;
        o.name = std::move(g.name);
        o.members = std::move(g.members);
        o.attrs = g.attrs;
        o.relative = g.relative;
        o.active = g.active;
        out = std::move(o);
    } else if (kind == "RemoveGroup") {
        RemoveGroup o;
        if (!rd.keys(v, "", {"op", "group"}) || !rd.id(v, "", "group", o.group)) return false;
        out = o;
    } else if (kind == "SetVca") {
        SetVca o;
        if (!read_named_set(rd, v, "", true, o.vca, o.name, o.members)) return false;
        out = std::move(o);
    } else if (kind == "RemoveVca") {
        RemoveVca o;
        if (!rd.keys(v, "", {"op", "vca"}) || !rd.id(v, "", "vca", o.vca)) return false;
        out = o;
    } else if (kind == "SetVcaMix") {
        SetVcaMix o;
        if (!rd.keys(v, "", {"op", "mix"}) ||
            !read_vca_mix_value(rd, *v.find("mix"), "mix", o.mix)) {
            return false;
        }
        out = o;
    } else if (kind == "SetFolder") {
        SetFolder o;
        if (!read_named_set(rd, v, "", true, o.folder, o.name, o.members)) return false;
        out = std::move(o);
    } else if (kind == "RemoveFolder") {
        RemoveFolder o;
        if (!rd.keys(v, "", {"op", "folder"}) || !rd.id(v, "", "folder", o.folder)) return false;
        out = o;
    } else {
        known = false;
        return false;
    }
    return true;
}

}  // namespace sonora::model::codec
