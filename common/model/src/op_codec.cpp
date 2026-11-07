// SPDX-License-Identifier: GPL-3.0-or-later
//
// One operation to bytes and back (op_codec.h): the dispatch to each
// family's encoder and decoder (op_codec_<family>.cpp, ADR-0059), and
// Compound, which holds members of every family.
#include "op_codec_detail.h"

namespace sonora::model {

namespace codec {

namespace {

Value encode_member(const Member& m) {
    return std::visit([](const auto& f) { return encode(f); }, m.families());
}

template <typename F, typename Out>
bool try_family(Reader& rd, const Value& v, const std::string& kind, Out& out, bool& known) {
    if (known) return false;
    F f;
    known = true;
    if (!decode(rd, v, kind, f, known)) return false;
    out = Out(std::move(f));
    return true;
}

// Any operation but Compound, as a Member or an Operation.
template <typename Out>
bool decode_leaf(Reader& rd, const Value& v, const std::string& kind, Out& out) {
    bool known = false;
    const bool ok = try_family<SessionOp>(rd, v, kind, out, known) ||
                    try_family<TrackOp>(rd, v, kind, out, known) ||
                    try_family<RegionOp>(rd, v, kind, out, known) ||
                    try_family<ClipOp>(rd, v, kind, out, known) ||
                    try_family<PlaylistOp>(rd, v, kind, out, known) ||
                    try_family<InsertOp>(rd, v, kind, out, known) ||
                    try_family<MixOp>(rd, v, kind, out, known) ||
                    try_family<GroupOp>(rd, v, kind, out, known) ||
                    try_family<MidiOp>(rd, v, kind, out, known);
    if (!known) {
        return rd.fail("op", "\"" + kind + "\" is not an operation this version knows");
    }
    return ok;
}

// Each member exactly as it would be alone; no label key when there is
// none, so the encoding stays canonical.
Value encode(const Compound& o) {
    Value v = with_kind("Compound");
    Value ops = Value::array();
    for (const Member& m : o.ops) ops.push(encode_member(m));
    v.set("ops", std::move(ops));
    if (!o.label.empty()) v.set("label", Value::string(o.label));
    return v;
}

// Members are refused by their JSON path, counted from 0 ("ops[3]: ..."),
// as the file and every other path in this codec count.
bool decode_compound(Reader& rd, const Value& v, Operation& out) {
    const bool labelled = v.is_object() && v.find("label") != nullptr;
    if (!(labelled ? rd.keys(v, "", {"op", "ops", "label"}) : rd.keys(v, "", {"op", "ops"}))) {
        return false;
    }
    const Value* ops = nullptr;
    if (!rd.array(v, "", "ops", ops)) return false;
    const std::size_t n = ops->items().size();
    if (n == 0) return rd.fail("ops", "a compound needs at least one operation; it is empty");
    if (n > kMaxCompoundMembers) {
        return rd.fail("ops", "at most " + std::to_string(kMaxCompoundMembers) +
                                  " operations in a compound; this one has " +
                                  std::to_string(n));
    }
    Compound c;
    c.ops.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Value& item = ops->items()[i];
        const std::string path = "ops[" + std::to_string(i) + "]";
        const Value* kind = item.is_object() ? item.find("op") : nullptr;
        if (kind == nullptr || !kind->is_string()) {
            return rd.fail(path, "must be an object whose \"op\" names the operation");
        }
        if (kind->text() == "Compound") return rd.fail(path, "a compound cannot hold a compound");
        std::string why;
        Reader member_reader(why, path);
        Member m;
        if (!decode_leaf(member_reader, item, kind->text(), m)) return rd.fail("", why);
        c.ops.push_back(std::move(m));
    }
    if (labelled) {
        if (!rd.text(v, "", "label", c.label)) return false;
        if (c.label.empty()) return rd.fail("label", "must not be empty; leave it out instead");
    }
    out = std::move(c);
    return true;
}

bool decode_value(Reader& rd, const Value& v, const std::string& kind, Operation& out) {
    if (kind == "Compound") return decode_compound(rd, v, out);
    return decode_leaf(rd, v, kind, out);
}

}  // namespace
}  // namespace codec

using namespace codec;

bool encode_operation(const Operation& op, std::string& out, std::string& error) {
    const Value v = std::visit([](const auto& f) { return encode(f); }, op.families());
    return json::write(v, out, error);
}

bool decode_operation(std::string_view bytes, Operation& out, std::string& error) {
    Value root;
    json::Error perr;
    if (!json::parse(bytes, root, perr)) {
        error = "operation: line " + std::to_string(perr.line) + ", column " +
                std::to_string(perr.column) + ": " + perr.message;
        return false;
    }
    if (!root.is_object() || root.find("op") == nullptr || !root.find("op")->is_string()) {
        error = "operation: must be an object whose \"op\" names the operation";
        return false;
    }
    Reader rd(error);
    Operation decoded;
    if (!decode_value(rd, root, root.find("op")->text(), decoded)) {
        error = "operation " + root.find("op")->text() + ": " + error;
        return false;
    }
    out = std::move(decoded);
    return true;
}

namespace {

// By flat index: family by family, in the order operations.h lists them.
const char* const kNames[] = {
    "AddSource", "SetMonitorBus", "SetSettings", "AddMarker", "MoveMarker", "RemoveMarker",
    "AddTrack", "RemoveTrack", "DuplicateTrack", "SetTrackName", "SetTrackColour", "ReorderTracks",
    "PlaceRegion", "RecordTake", "RecordLoopTake", "MoveRegion", "TrimRegion", "SplitRegion",
    "RemoveRegion", "ShiftRegions",
    "SetFade", "SetCrossfade", "RemoveCrossfade", "SetRegionName", "SetRegionColour",
    "SetRegionGain", "SetClipGroup", "SetRegionSources",
    "AddPlaylist", "SetActivePlaylist", "DuplicatePlaylist", "SetPlaylistName", "RemovePlaylist",
    "AddInsert", "RemoveInsert", "MoveInsert", "SetInsertActive", "ReplaceInsert",
    "SetInsertBypass", "SetInsertParams", "SetInsertState",
    "SetMix", "AddBus", "RemoveBus", "AddSend", "RemoveSend", "SetSendMix", "SetBusMix",
    "SetOutput", "EditAutomation", "SetBusPath",
    "SetGroup", "RemoveGroup", "SetVca", "RemoveVca", "SetVcaMix", "SetFolder", "RemoveFolder",
    "AddMidiSource", "PlaceMidiRegion", "EditMidi", "SetMidiRouting",
    "Compound"};
static_assert(sizeof kNames / sizeof kNames[0] == Operation::kinds,
              "a name for every operation");

}  // namespace

const char* operation_name(const Operation& op) { return kNames[op.index()]; }

// Member's leaves are Operation's, at the same flat indices.
const char* member_name(const Member& m) { return kNames[m.index()]; }

std::string display_name(const Operation& op) {
    const auto* c = get_if<Compound>(&op);
    if (c == nullptr) return operation_name(op);
    if (!c->label.empty()) return c->label;
    for (const Member& m : c->ops) {
        if (member_is_undoable(m)) return member_name(m);
    }
    return c->ops.empty() ? "Compound" : member_name(c->ops.front());
}

}  // namespace sonora::model
