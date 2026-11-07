// SPDX-License-Identifier: GPL-3.0-or-later
//
// Clip groups and loops, and the sample edits' model half (ADR-0052):
// SetClipGroup and SetRegionSources apply, refuse, undo and cross the codec,
// the undo window and the session file; a version 23 file may not hold a
// group.
#include <optional>

#include "model_test.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/session_file.h"

using namespace model_test;

namespace {

struct Rig {
    History h{new_session(settings())};
    Ids ids{h.current()};
    AddTrack track;
    AddSource src;
    Id a = 0, b = 0, c = 0;
};

bool build(Rig& r) {
    r.track = add_track(r.ids, "Vox");
    std::string e;
    if (!r.h.apply(r.track, e)) return false;
    r.src = add_source(r.ids, 480000, 1, r.track.track);
    if (!r.h.apply(r.src, e)) return false;
    r.a = r.ids();
    r.b = r.ids();
    r.c = r.ids();
    const Id s = r.src.source.id;
    return r.h.apply(PlaceRegion{r.track.playlist, region(r.a, s, 0, 1000, 0), 0}, e) &&
           r.h.apply(PlaceRegion{r.track.playlist, region(r.b, s, 1000, 1000, 1000), 0}, e) &&
           r.h.apply(PlaceRegion{r.track.playlist, region(r.c, s, 5000, 1000, 5000), 0}, e);
}

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

int test_group_and_ungroup() {
    Rig r;
    CHECK(build(r));
    const Id pl = r.track.playlist;
    CHECK_APPLY(r.h, (SetClipGroup{pl, r.a, {{r.a, 0}, {r.b, 0}}}));
    CHECK(find(r.h.current(), r.a)->clip_group == r.a && find(r.h.current(), r.b)->clip_group == r.a);
    CHECK(find(r.h.current(), r.c)->clip_group == 0);
    // One step back.
    CHECK(r.h.undo());
    CHECK(find(r.h.current(), r.a)->clip_group == 0 && find(r.h.current(), r.b)->clip_group == 0);
    CHECK(r.h.redo());

    // Refusals leave the document alone.
    CHECK(refused(r.h, SetClipGroup{pl, r.a, {}}, "at least one"));
    CHECK(refused(r.h, SetClipGroup{pl, 999, {{999, 0}}}, "999"));
    CHECK(refused(r.h, SetClipGroup{pl, r.c, {{r.a, 0}, {r.b, 0}}}, "named by one of its regions"));
    CHECK(refused(r.h, SetClipGroup{pl, r.a, {{r.a, 0}, {r.a, 0}}}, "twice"));
    CHECK(refused(r.h, SetClipGroup{pl, r.a, {{r.a, 1}, {r.b, 1}}}, "not a repeat"));
    // The group already holds b: naming only a would leave b behind.
    CHECK(refused(r.h, SetClipGroup{pl, r.a, {{r.a, 0}, {r.c, 0}}}, "already holds"));
    CHECK(refused(r.h, SetClipGroup{pl, 0, {{r.a, 2}}}, "no loop number"));

    // Ungrouping frees them and clears any loop number.
    CHECK_APPLY(r.h, (SetClipGroup{pl, r.a, {{r.a, 0}, {r.b, 3}}}));
    CHECK(find(r.h.current(), r.b)->loop == 3);
    CHECK_APPLY(r.h, (SetClipGroup{pl, 0, {{r.a, 0}, {r.b, 0}}}));
    CHECK(find(r.h.current(), r.a)->clip_group == 0 && find(r.h.current(), r.b)->loop == 0);
    return 0;
}

int test_loop_region_rules() {
    Rig r;
    CHECK(build(r));
    // A repeat must belong to a group; a region placed whole carries both.
    Region lone = region(r.ids(), r.src.source.id, 8000, 500, 0);
    lone.loop = 1;
    CHECK(refused(r.h, PlaceRegion{r.track.playlist, lone, 0}, "clip group"));
    lone.clip_group = r.a;
    CHECK_APPLY(r.h, (PlaceRegion{r.track.playlist, lone, 0}));
    CHECK(find(r.h.current(), lone.id)->loop == 1);
    // Equality sees both fields.
    Region x = lone, y = lone;
    y.loop = 2;
    CHECK(!(x == y));
    y = lone;
    y.clip_group = 5;
    CHECK(!(x == y));
    return 0;
}

int test_sources() {
    Rig r;
    CHECK(build(r));
    const Id pl = r.track.playlist;
    std::string error;
    AddSource other = add_source(r.ids, 3000, 1, r.track.track);
    CHECK_APPLY(r.h, other);
    const std::vector<GainPoint> line{{100, -3.0f}};
    CHECK_APPLY(r.h, (SetRegionSources{pl, r.b, {{other.source.id, 0}}, 500, line}));
    const Region* b = find(r.h.current(), r.b);
    CHECK(b->channels[0].source == other.source.id && b->source_offset == 500 && b->gain_line == line);
    CHECK(b->position == 1000 && b->length == 1000);
    CHECK(r.h.undo());
    b = find(r.h.current(), r.b);
    CHECK(b->channels[0].source == r.src.source.id && b->source_offset == 1000 && b->gain_line.empty());
    CHECK(r.h.redo());

    CHECK(refused(r.h, SetRegionSources{pl, 999, {{other.source.id, 0}}, 0, {}}, "999"));
    CHECK(refused(r.h, SetRegionSources{pl, r.b, {{12345, 0}}, 0, {}}, "does not exist"));
    CHECK(refused(r.h, SetRegionSources{pl, r.b, {{other.source.id, 3}}, 0, {}}, "no channel"));
    CHECK(refused(r.h, SetRegionSources{pl, r.b, {{other.source.id, 0}}, 2500, {}}, "past the end"));
    CHECK(refused(r.h, SetRegionSources{pl, r.b, {}, 0, {}}, "source entries"));
    CHECK(refused(r.h, SetRegionSources{pl, r.b, {{other.source.id, 0}}, 0, {{5, 0.0f}, {5, 1.0f}}},
                  "increasing"));
    return 0;
}

int test_codec_window_and_file() {
    Rig r;
    CHECK(build(r));
    const Id pl = r.track.playlist;
    std::string error;
    CHECK_APPLY(r.h, (SetClipGroup{pl, r.a, {{r.a, 0}, {r.b, 1}}}));
    Operation ops[] = {
        SetClipGroup{pl, r.a, {{r.a, 0}, {r.b, 1}}},
        SetClipGroup{pl, 0, {{r.a, 0}}},
        SetRegionSources{pl, r.b, {{r.src.source.id, 0}}, 1000, {{0, 1.0f}}},
    };
    for (const Operation& op : ops) {
        std::string bytes;
        CHECK(encode_operation(op, bytes, error));
        Operation back;
        CHECK(decode_operation(bytes, back, error));
        CHECK(operation_name(back) == std::string(operation_name(op)));
        Snapshot x, y;
        const bool ax = apply(r.h.current(), op, x, error);
        const bool ay = apply(r.h.current(), back, y, error);
        CHECK(ax == ay && (!ax || x == y));
    }
    Operation out;
    CHECK(!decode_operation(R"({"op": "SetClipGroup", "playlist": 1, "group": 2})", out, error));
    CHECK(mentions(error, "members"));
    CHECK(!decode_operation(R"({"op": "SetRegionSources", "playlist": 1, "region": 2, "source": []})",
                            out, error));
    CHECK(std::string(operation_name(Operation{SetClipGroup{}})) == "SetClipGroup");
    CHECK(std::string(operation_name(Operation{SetRegionSources{}})) == "SetRegionSources");

    // A region placed with a group and a loop crosses whole.
    Region looped = region(r.ids(), r.src.source.id, 9000, 100, 0);
    looped.clip_group = r.a;
    looped.loop = 2;
    std::string bytes;
    CHECK(encode_operation(PlaceRegion{pl, looped, 0}, bytes, error));
    Operation back;
    CHECK(decode_operation(bytes, back, error));
    CHECK(get<PlaceRegion>(back).region.clip_group == r.a && get<PlaceRegion>(back).region.loop == 2);

    // The undo window.
    WindowParts w;
    CHECK(encode_window(r.h, w, error));
    std::optional<History> replica;
    CHECK(decode_window(w, replica, error));
    CHECK(replica.has_value() && replica->current() == r.h.current());

    // The session file keeps the group, and writes none for a clip without.
    std::string file;
    CHECK(serialize(r.h.current(), WriteInfo{"test", 1}, file, error));
    CHECK(file.find("\"clip_group\"") != std::string::npos && file.find("\"loop\": 1") != std::string::npos);
    Loaded loaded;
    LoadError le;
    CHECK(deserialize(file, loaded, le));
    CHECK(loaded.file_version == kFormatVersion && kFormatVersion == 25);
    CHECK(loaded.snapshot.edit->tracks[0]->playlists[0]->regions ==
          r.h.current().edit->tracks[0]->playlists[0]->regions);
    std::string again;
    CHECK(serialize(loaded.snapshot, WriteInfo{"test", 1}, again, error) && again == file);
    // A file that holds none carries no key.
    Rig plain;
    CHECK(build(plain));
    std::string plain_file;
    CHECK(serialize(plain.h.current(), WriteInfo{"test", 1}, plain_file, error));
    CHECK(plain_file.find("clip_group") == std::string::npos);

    // A version 23 file may not hold a group.
    std::string old = file;
    const std::size_t v = old.find("\"sonora_session\": 25");
    CHECK(v != std::string::npos);
    old.replace(v, 20, "\"sonora_session\": 23");
    CHECK(!deserialize(old, loaded, le));
    CHECK(mentions(le.message, "clip_group"));
    // A repeat with no group is refused whatever the version.
    std::string bad = file;
    const std::size_t g = bad.find("\"clip_group\"");
    CHECK(g != std::string::npos);
    bad.replace(g, 12, "\"clip_gr0up\"");
    CHECK(!deserialize(bad, loaded, le));
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_group_and_ungroup();
    failed += test_loop_region_rules();
    failed += test_sources();
    failed += test_codec_window_and_file();
    if (failed == 0) std::printf("ok\n");
    return failed == 0 ? 0 : 1;
}
