// SPDX-License-Identifier: GPL-3.0-or-later
//
// Clip gain (Pro Tools' Clip Gain): SetRegionGain replaces a region's level
// and gain line as one undo step; the line is in source frames, so a move,
// trim or split keeps it; the codec and the undo window carry it.
#include <optional>

#include "model_test.h"
#include "sonora/model/op_codec.h"

using namespace model_test;

namespace {

bool refused(History& h, const Operation& op, const char* says) {
    const Snapshot before = h.current();
    std::string error;
    if (h.apply(op, error)) {
        std::fprintf(stderr, "  applied, expected \"%s\"\n", says);
        return false;
    }
    return mentions(error, says) && h.current() == before;
}

const Region* find(const Snapshot& s, Id region) {
    for (const auto& t : s.edit->tracks) {
        for (const auto& p : t->playlists) {
            for (const Region& r : p->regions) {
                if (r.id == region) return &r;
            }
        }
    }
    return nullptr;
}

int test_clip_gain() {
    History h{new_session(settings())};
    Ids ids(h.current());
    const AddTrack vox = add_track(ids, "Vox");
    CHECK_APPLY(h, vox);
    const AddSource src = add_source(ids, 480000, 1, vox.track);
    CHECK_APPLY(h, src);
    const Id rid = ids();
    CHECK_APPLY(h, (PlaceRegion{vox.playlist, region(rid, src.source.id, 1000, 48000, 2000), 0}));

    const std::vector<GainPoint> line{{2000, -6.0f}, {30000, 3.0f}};
    CHECK_APPLY(h, (SetRegionGain{vox.playlist, rid, -2.0f, line}));
    const Region* r = find(h.current(), rid);
    CHECK(r != nullptr && r->gain_db == -2.0f && r->gain_line == line);

    // One step back restores both the level and the line.
    std::string error;
    CHECK(h.undo(error));
    r = find(h.current(), rid);
    CHECK(r != nullptr && r->gain_db == 0.0f && r->gain_line.empty());
    CHECK(h.redo(error));
    CHECK(find(h.current(), rid)->gain_line == line);

    // The line is in source frames: a move leaves it alone, a split gives
    // both halves the whole line.
    CHECK_APPLY(h, (MoveRegion{vox.playlist, rid, 5000, 0}));
    CHECK(find(h.current(), rid)->gain_line == line);
    const Id right = ids();
    CHECK_APPLY(h, (SplitRegion{vox.playlist, rid, 20000, right}));
    CHECK(find(h.current(), rid)->gain_line == line);
    CHECK(find(h.current(), right)->gain_line == line);
    CHECK(find(h.current(), right)->gain_db == -2.0f);

    // Refusals leave the document alone.
    CHECK(refused(h, SetRegionGain{vox.playlist, 999, 0.0f, {}}, "999"));
    CHECK(refused(h, SetRegionGain{vox.playlist, rid, 25.0f, {}}, "gain_db"));
    CHECK(refused(h, SetRegionGain{vox.playlist, rid, 0.0f, {{5, 0.0f}, {5, 1.0f}}},
                  "increasing"));
    CHECK(refused(h, SetRegionGain{vox.playlist, rid, 0.0f, {{-1, 0.0f}}}, "negative"));
    CHECK(refused(h, SetRegionGain{vox.playlist, rid, 0.0f, {{1, 99.0f}}}, "db"));
    std::vector<GainPoint> many;
    for (std::size_t i = 0; i <= kMaxGainPoints; ++i) many.push_back({static_cast<Frames>(i), 0.0f});
    CHECK(refused(h, SetRegionGain{vox.playlist, rid, 0.0f, many}, "more than"));

    // The codec: the operation, and a region placed with a line.
    Region withline = region(ids(), src.source.id, 200000, 1000, 0);
    withline.gain_line = {{0, 1.5f}};
    for (const Operation& op : {Operation{SetRegionGain{vox.playlist, rid, 1.0f, line}},
                                Operation{PlaceRegion{vox.playlist, withline, 0}}}) {
        std::string bytes;
        CHECK(encode_operation(op, bytes, error));
        Operation back;
        CHECK(decode_operation(bytes, back, error));
        Snapshot a, b;
        CHECK(apply(h.current(), op, a, error));
        CHECK(apply(h.current(), back, b, error));
        CHECK(a == b);
    }
    Operation out;
    CHECK(!decode_operation(R"({"op": "SetRegionGain", "playlist": 1, "region": 2, "gain_db": 0})",
                            out, error));
    CHECK(mentions(error, "gain_line"));
    CHECK(std::string(operation_name(Operation{SetRegionGain{}})) == "SetRegionGain");

    // The undo window: the replica sees the line the engine has.
    WindowParts w;
    CHECK(encode_window(h, w, error));
    std::optional<History> replica;
    CHECK(decode_window(w, replica, error));
    CHECK(replica.has_value() && replica->current() == h.current());
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_clip_gain();
    if (failed == 0) std::printf("ok\n");
    return failed == 0 ? 0 : 1;
}
