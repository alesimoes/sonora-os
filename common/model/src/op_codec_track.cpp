// SPDX-License-Identifier: GPL-3.0-or-later
//
// The op codec (op_codec.h) for the family TrackOp (ADR-0059):
// tracks: add, remove, duplicate, name, colour, order.
#include "op_codec_detail.h"

namespace sonora::model::codec {

namespace {

struct Encoder {
    Value operator()(const AddTrack& o) const {
        Value v = with_kind("AddTrack");
        v.set("track", id_value(o.track));
        v.set("name", Value::string(o.name));
        v.set("channels", integer(o.channels));
        v.set("playlist", id_value(o.playlist));
        v.set("playlist_name", Value::string(o.playlist_name));
        v.set("mix", mix_value(o.mix));
        v.set("kind", name_of(kTrackKinds, o.kind));
        return v;
    }
    Value operator()(const RemoveTrack& o) const {
        Value v = with_kind("RemoveTrack");
        v.set("track", id_value(o.track));
        return v;
    }
    Value operator()(const DuplicateTrack& o) const {
        Value v = with_kind("DuplicateTrack");
        v.set("source", id_value(o.source));
        v.set("track", id_value(o.track));
        v.set("name", Value::string(o.name));
        v.set("all_playlists", Value::boolean(o.all_playlists));
        v.set("inserts", Value::boolean(o.inserts));
        v.set("sends", Value::boolean(o.sends));
        v.set("automation", Value::boolean(o.automation));
        return v;
    }
    Value operator()(const SetTrackName& o) const {
        Value v = with_kind("SetTrackName");
        v.set("track", id_value(o.track));
        v.set("name", Value::string(o.name));
        return v;
    }
    Value operator()(const SetTrackColour& o) const {
        Value v = with_kind("SetTrackColour");
        v.set("track", id_value(o.track));
        v.set("colour", integer(o.colour));
        return v;
    }
    Value operator()(const ReorderTracks& o) const {
        Value v = with_kind("ReorderTracks");
        Value& order = v.set("order", Value::array());
        for (const Id id : o.order) order.push(id_value(id));
        return v;
    }
};

}  // namespace

Value encode(const TrackOp& op) { return std::visit(Encoder{}, op); }

bool decode(Reader& rd, const Value& v, const std::string& kind, TrackOp& out,
            bool& known) {
    known = true;
    if (kind == "AddTrack") {
        AddTrack o;
        if (!rd.keys(v, "", {"op", "track", "name", "channels", "playlist", "playlist_name",
                             "mix", "kind"}) ||
            !rd.id(v, "", "track", o.track) || !rd.text(v, "", "name", o.name) ||
            !rd.small(v, "", "channels", o.channels) || !rd.id(v, "", "playlist", o.playlist) ||
            !rd.text(v, "", "playlist_name", o.playlist_name) || !read_mix(rd, v, "", o.mix) ||
            !rd.choice(v, "", "kind", kTrackKinds, o.kind)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "RemoveTrack") {
        RemoveTrack o;
        if (!rd.keys(v, "", {"op", "track"}) || !rd.id(v, "", "track", o.track)) return false;
        out = o;
    } else if (kind == "DuplicateTrack") {
        DuplicateTrack o;
        if (!rd.keys(v, "", {"op", "source", "track", "name", "all_playlists", "inserts",
                             "sends", "automation"}) ||
            !rd.id(v, "", "source", o.source) || !rd.id(v, "", "track", o.track) ||
            !rd.text(v, "", "name", o.name) ||
            !rd.flag(v, "", "all_playlists", o.all_playlists) ||
            !rd.flag(v, "", "inserts", o.inserts) || !rd.flag(v, "", "sends", o.sends) ||
            !rd.flag(v, "", "automation", o.automation)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "SetTrackName") {
        SetTrackName o;
        if (!rd.keys(v, "", {"op", "track", "name"}) || !rd.id(v, "", "track", o.track) ||
            !rd.text(v, "", "name", o.name)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "SetTrackColour") {
        SetTrackColour o;
        if (!rd.keys(v, "", {"op", "track", "colour"}) || !rd.id(v, "", "track", o.track) ||
            !read_colour(rd, v, "", false, o.colour)) {
            return false;
        }
        out = o;
    } else if (kind == "ReorderTracks") {
        ReorderTracks o;
        const Value* order = nullptr;
        if (!rd.keys(v, "", {"op", "order"}) || !rd.array(v, "", "order", order)) return false;
        for (const Value& e : order->items()) {
            std::int64_t n = 0;
            if (!e.is_integer_lexeme() || !e.to_int64(n) || n < 0 || n > kMaxInteger) {
                return rd.fail("order", "must hold track ids");
            }
            o.order.push_back(static_cast<Id>(n));
        }
        out = std::move(o);
    } else {
        known = false;
        return false;
    }
    return true;
}

}  // namespace sonora::model::codec
