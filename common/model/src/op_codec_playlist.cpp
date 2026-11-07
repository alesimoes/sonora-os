// SPDX-License-Identifier: GPL-3.0-or-later
//
// The op codec (op_codec.h) for the family PlaylistOp (ADR-0059):
// a track's playlists.
#include "op_codec_detail.h"

namespace sonora::model::codec {

namespace {

struct Encoder {
    Value operator()(const AddPlaylist& o) const {
        Value v = with_kind("AddPlaylist");
        v.set("track", id_value(o.track));
        v.set("playlist", id_value(o.playlist));
        v.set("name", Value::string(o.name));
        if (o.freeze) v.set("freeze", freeze_value(*o.freeze));
        return v;
    }
    Value operator()(const SetActivePlaylist& o) const {
        Value v = with_kind("SetActivePlaylist");
        v.set("track", id_value(o.track));
        v.set("playlist", id_value(o.playlist));
        return v;
    }
    Value operator()(const DuplicatePlaylist& o) const {
        Value v = with_kind("DuplicatePlaylist");
        v.set("track", id_value(o.track));
        v.set("source", id_value(o.source));
        v.set("playlist", id_value(o.playlist));
        v.set("name", Value::string(o.name));
        return v;
    }
    Value operator()(const SetPlaylistName& o) const {
        Value v = with_kind("SetPlaylistName");
        v.set("track", id_value(o.track));
        v.set("playlist", id_value(o.playlist));
        v.set("name", Value::string(o.name));
        return v;
    }
    Value operator()(const RemovePlaylist& o) const {
        Value v = with_kind("RemovePlaylist");
        v.set("track", id_value(o.track));
        v.set("playlist", id_value(o.playlist));
        return v;
    }
};

}  // namespace

Value encode(const PlaylistOp& op) { return std::visit(Encoder{}, op); }

bool decode(Reader& rd, const Value& v, const std::string& kind, PlaylistOp& out,
            bool& known) {
    known = true;
    if (kind == "AddPlaylist") {
        AddPlaylist o;
        const bool frozen = v.is_object() && v.find("freeze") != nullptr;
        if (!(frozen ? rd.keys(v, "", {"op", "track", "playlist", "name", "freeze"})
                     : rd.keys(v, "", {"op", "track", "playlist", "name"})) ||
            !rd.id(v, "", "track", o.track) || !rd.id(v, "", "playlist", o.playlist) ||
            !rd.text(v, "", "name", o.name) || (frozen && !read_freeze(rd, v, "", o.freeze))) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "SetActivePlaylist") {
        SetActivePlaylist o;
        if (!rd.keys(v, "", {"op", "track", "playlist"}) || !rd.id(v, "", "track", o.track) ||
            !rd.id(v, "", "playlist", o.playlist)) {
            return false;
        }
        out = o;
    } else if (kind == "DuplicatePlaylist") {
        DuplicatePlaylist o;
        if (!rd.keys(v, "", {"op", "track", "source", "playlist", "name"}) ||
            !rd.id(v, "", "track", o.track) || !rd.id(v, "", "source", o.source) ||
            !rd.id(v, "", "playlist", o.playlist) || !rd.text(v, "", "name", o.name)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "SetPlaylistName") {
        SetPlaylistName o;
        if (!rd.keys(v, "", {"op", "track", "playlist", "name"}) ||
            !rd.id(v, "", "track", o.track) || !rd.id(v, "", "playlist", o.playlist) ||
            !rd.text(v, "", "name", o.name)) {
            return false;
        }
        out = std::move(o);
    } else if (kind == "RemovePlaylist") {
        RemovePlaylist o;
        if (!rd.keys(v, "", {"op", "track", "playlist"}) || !rd.id(v, "", "track", o.track) ||
            !rd.id(v, "", "playlist", o.playlist)) {
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
