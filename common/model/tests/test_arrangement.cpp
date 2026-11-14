// SPDX-License-Identifier: GPL-3.0-or-later
//
// Arrangement sections (ADR-0116): copy and move a range marker's audio and
// markers as one Compound, insert-style, with its refusals and one undo step.
#include <cstdio>
#include <string>
#include <vector>

#include "model_test.h"
#include "sonora/model/arrangement.h"

using namespace model_test;

namespace {

// Two audio tracks and a MIDI one, which every section edit must skip.
// Track A: a [0,100) b [100,200) c [200,300); track B: e [250,350).
// Markers: Intro at 50, the section Verse over [100,200), Hook at 150,
// End at 350.
struct Fixture {
    History h{new_session(settings())};
    Id src = 0, pa = 0, pb = 0, a = 0, b = 0, c = 0, e = 0;
    Id intro = 0, verse = 0, hook = 0, end = 0;
};

const Marker* marker(const Snapshot& s, Id id) {
    for (const Marker& m : s.edit->markers->markers) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

const Marker* marker_named(const Snapshot& s, const std::string& name, Id except) {
    for (const Marker& m : s.edit->markers->markers) {
        if (m.name == name && m.id != except) return &m;
    }
    return nullptr;
}

const Region* region_at(const Playlist& p, Frames position) {
    for (const Region& r : p.regions) {
        if (r.position == position) return &r;
    }
    return nullptr;
}

int build(Fixture& f) {
    Ids ids(f.h.current());
    const AddTrack ta = add_track(ids, "A");
    const AddTrack tb = add_track(ids, "B");
    CHECK_APPLY(f.h, ta);
    CHECK_APPLY(f.h, tb);
    AddTrack keys;
    keys.track = ids();
    keys.playlist = ids();
    keys.name = "Keys";
    keys.playlist_name = "Keys.01";
    keys.kind = TrackKind::Midi;
    keys.channels = 0;
    CHECK_APPLY(f.h, keys);
    const AddSource s = add_source(ids, 1000);
    CHECK_APPLY(f.h, s);
    f.src = s.source.id;
    f.pa = ta.playlist;
    f.pb = tb.playlist;
    f.a = ids();
    CHECK_APPLY(f.h, (PlaceRegion{f.pa, region(f.a, f.src, 0, 100, 0), 0}));
    f.b = ids();
    CHECK_APPLY(f.h, (PlaceRegion{f.pa, region(f.b, f.src, 100, 100, 100), 0}));
    f.c = ids();
    CHECK_APPLY(f.h, (PlaceRegion{f.pa, region(f.c, f.src, 200, 100, 200), 0}));
    f.e = ids();
    CHECK_APPLY(f.h, (PlaceRegion{f.pb, region(f.e, f.src, 250, 100, 0), 0}));
    f.intro = ids();
    CHECK_APPLY(f.h, (AddMarker{Marker{f.intro, 50, "Intro", 0}}));
    f.verse = ids();
    CHECK_APPLY(f.h, (AddMarker{Marker{f.verse, 100, "Verse", 100}}));
    f.hook = ids();
    CHECK_APPLY(f.h, (AddMarker{Marker{f.hook, 150, "Hook", 0}}));
    f.end = ids();
    CHECK_APPLY(f.h, (AddMarker{Marker{f.end, 350, "End", 0}}));
    return 0;
}

// Every id the plan introduces, in member order.
std::vector<Id> new_ids(const Compound& c) {
    std::vector<Id> out;
    for (const Member& m : c.ops) {
        if (const auto* x = get_if<SplitRegion>(&m)) out.push_back(x->new_right_id);
        if (const auto* x = get_if<PlaceRegion>(&m)) {
            out.push_back(x->region.id);
            if (x->split_right_id != 0) out.push_back(x->split_right_id);
        }
        if (const auto* x = get_if<MoveRegion>(&m)) {
            if (x->split_right_id != 0) out.push_back(x->split_right_id);
        }
        if (const auto* x = get_if<AddMarker>(&m)) out.push_back(x->marker.id);
    }
    return out;
}

bool ids_rise_from(const Compound& c, Id next_id) {
    Id last = next_id - 1;
    for (const Id id : new_ids(c)) {
        if (id <= last) {
            std::fprintf(stderr, "  id %llu after %llu\n", static_cast<unsigned long long>(id),
                         static_cast<unsigned long long>(last));
            return false;
        }
        last = id;
    }
    return true;
}

// Plans, applies as one step, checks the ids and the invariants, then
// checks one undo brings the edit back.
int apply_plan(Fixture& f, const SectionEdit& edit, Snapshot& after) {
    const Snapshot before = f.h.current();
    const SectionPlan plan = plan_section_edit(before, edit, before.next_id);
    if (!plan.ok()) std::fprintf(stderr, "  refused: %s\n", plan.refusal.c_str());
    CHECK(plan.ok());
    CHECK(!plan.compound.label.empty());
    CHECK(plan.compound.ops.size() <= kMaxCompoundMembers);
    CHECK(ids_rise_from(plan.compound, before.next_id));
    CHECK_APPLY(f.h, plan.compound);
    after = f.h.current();
    CHECK(invariants_hold(after));
    CHECK(f.h.undo());
    CHECK(*f.h.current().edit == *before.edit);
    CHECK(f.h.redo());
    return 0;
}

int test_copy_before() {
    Fixture f;
    if (build(f) != 0) return 1;
    Snapshot s;
    if (apply_plan(f, SectionEdit{f.verse, 0, SectionMode::Copy}, s) != 0) return 1;
    const Playlist& a = playlist_of(s, 0);
    CHECK(a.regions.size() == 4);
    const Region* copy = region_at(a, 0);
    CHECK(copy != nullptr && copy->id != f.b && copy->source_offset == 100 &&
          copy->length == 100);
    CHECK(find(a, f.a)->position == 100);
    CHECK(find(a, f.b)->position == 200);
    CHECK(find(a, f.c)->position == 300);
    CHECK(find(playlist_of(s, 1), f.e)->position == 350);
    CHECK(marker(s, f.intro)->position == 150);
    CHECK(marker(s, f.verse)->position == 200);
    CHECK(marker(s, f.hook)->position == 250);
    CHECK(marker(s, f.end)->position == 450);
    const Marker* verse2 = marker_named(s, "Verse", f.verse);
    CHECK(verse2 != nullptr && verse2->position == 0 && verse2->length == 100);
    const Marker* hook2 = marker_named(s, "Hook", f.hook);
    CHECK(hook2 != nullptr && hook2->position == 50);
    return 0;
}

int test_copy_after_splits_destination() {
    Fixture f;
    if (build(f) != 0) return 1;
    Snapshot s;
    // 300 cuts B's e [250,350); its right part shifts with what follows.
    if (apply_plan(f, SectionEdit{f.verse, 300, SectionMode::Copy}, s) != 0) return 1;
    const Playlist& a = playlist_of(s, 0);
    CHECK(a.regions.size() == 4);
    CHECK(find(a, f.b)->position == 100);
    const Region* copy = region_at(a, 300);
    CHECK(copy != nullptr && copy->source_offset == 100);
    const Playlist& b = playlist_of(s, 1);
    CHECK(b.regions.size() == 2);
    CHECK(find(b, f.e)->position == 250 && find(b, f.e)->length == 50);
    const Region* right = region_at(b, 400);
    CHECK(right != nullptr && right->length == 50 && right->source_offset == 50);
    CHECK(marker(s, f.hook)->position == 150);
    CHECK(marker(s, f.end)->position == 450);
    CHECK(marker_named(s, "Hook", f.hook)->position == 350);
    CHECK(marker_named(s, "Verse", f.verse)->position == 300);
    return 0;
}

int test_move_later() {
    Fixture f;
    if (build(f) != 0) return 1;
    Snapshot s;
    if (apply_plan(f, SectionEdit{f.verse, 300, SectionMode::Move}, s) != 0) return 1;
    const Playlist& a = playlist_of(s, 0);
    CHECK(a.regions.size() == 3);
    CHECK(find(a, f.a)->position == 0);
    CHECK(find(a, f.c)->position == 100);
    // Moved, not copied: the section keeps its region's id.
    CHECK(find(a, f.b)->position == 200 && find(a, f.b)->source_offset == 100);
    const Playlist& b = playlist_of(s, 1);
    CHECK(b.regions.size() == 2);
    CHECK(find(b, f.e)->position == 150 && find(b, f.e)->length == 50);
    CHECK(region_at(b, 300) != nullptr);
    CHECK(marker(s, f.intro)->position == 50);
    CHECK(marker(s, f.verse)->position == 200 && marker(s, f.verse)->length == 100);
    CHECK(marker(s, f.hook)->position == 250);
    CHECK(marker(s, f.end)->position == 350);
    CHECK(s.edit->markers->markers.size() == 4);
    return 0;
}

// One region under the section's start, end and destination: three splits,
// each part keeping the audio it played.
int test_move_earlier_straddling() {
    Fixture f;
    if (build(f) != 0) return 1;
    Ids ids(f.h.current());
    const Id long_region = ids();
    CHECK_APPLY(f.h, (PlaceRegion{f.pb, region(long_region, f.src, 0, 400, 0), ids()}));
    const Id chorus = ids();
    CHECK_APPLY(f.h, (AddMarker{Marker{chorus, 200, "Chorus", 100}}));
    Snapshot s;
    if (apply_plan(f, SectionEdit{chorus, 100, SectionMode::Move}, s) != 0) return 1;
    const Playlist& b = playlist_of(s, 1);
    CHECK(b.regions.size() == 4);
    const Frames expect[4][2] = {{0, 0}, {100, 200}, {200, 100}, {300, 300}};
    for (const auto& x : expect) {
        const Region* r = region_at(b, x[0]);
        CHECK(r != nullptr && r->length == 100 && r->source_offset == x[1]);
    }
    CHECK(find(b, long_region)->position == 0);
    CHECK(marker(s, chorus)->position == 100);
    const Playlist& a = playlist_of(s, 0);
    CHECK(find(a, f.c)->position == 100 && find(a, f.b)->position == 200);
    return 0;
}

int test_markers_only_section() {
    Fixture f;
    if (build(f) != 0) return 1;
    Ids ids(f.h.current());
    const Id outro = ids();
    CHECK_APPLY(f.h, (AddMarker{Marker{outro, 600, "Outro", 100}}));
    const Id tag = ids();
    CHECK_APPLY(f.h, (AddMarker{Marker{tag, 650, "Tag", 0}}));
    Snapshot s;
    if (apply_plan(f, SectionEdit{outro, 900, SectionMode::Copy}, s) != 0) return 1;
    CHECK(marker_named(s, "Tag", tag)->position == 950);
    CHECK(marker_named(s, "Outro", outro)->position == 900);
    CHECK(playlist_of(s, 0).regions.size() == 3);
    return 0;
}

int refused(const Snapshot& s, const SectionEdit& e, const char* words) {
    const SectionPlan plan = plan_section_edit(s, e, s.next_id);
    CHECK(!plan.ok());
    CHECK(plan.compound.ops.empty());
    CHECK(mentions(plan.refusal, words));
    return 0;
}

int test_refusals() {
    Fixture f;
    if (build(f) != 0) return 1;
    const Snapshot& s = f.h.current();
    if (refused(s, {f.verse, 150, SectionMode::Copy}, "inside section") != 0) return 1;
    if (refused(s, {f.verse, 150, SectionMode::Move}, "inside section") != 0) return 1;
    if (refused(s, {f.verse, 100, SectionMode::Move}, "already there") != 0) return 1;
    if (refused(s, {f.verse, 200, SectionMode::Move}, "already there") != 0) return 1;
    if (refused(s, {f.hook, 0, SectionMode::Copy}, "not a section") != 0) return 1;
    if (refused(s, {9999, 0, SectionMode::Copy}, "does not exist") != 0) return 1;
    if (refused(s, {f.verse, -1, SectionMode::Copy}, "before the start") != 0) return 1;

    Ids ids(f.h.current());
    const Id empty = ids();
    CHECK_APPLY(f.h, (AddMarker{Marker{empty, 1000, "Empty", 100}}));
    if (refused(f.h.current(), {empty, 0, SectionMode::Copy}, "no audio and no markers") != 0) {
        return 1;
    }
    const Id far = ids();
    CHECK_APPLY(f.h, (AddMarker{Marker{far, kMaxInteger - 50, "Far", 0}}));
    if (refused(f.h.current(), {f.verse, 0, SectionMode::Copy}, "past the end of the timeline") !=
        0) {
        return 1;
    }
    return 0;
}

// 300 regions inside the section: 300 copies and the section's own marker
// are more than one compound holds, and the message says how many.
int test_over_cap() {
    History h{new_session(settings())};
    Ids ids(h.current());
    const AddTrack t = add_track(ids, "A");
    CHECK_APPLY(h, t);
    const AddSource s = add_source(ids, 1000);
    CHECK_APPLY(h, s);
    for (Frames k = 0; k < 300; ++k) {
        CHECK_APPLY(h, (PlaceRegion{t.playlist, region(ids(), s.source.id, k, 1, k), 0}));
    }
    const Id big = ids();
    CHECK_APPLY(h, (AddMarker{Marker{big, 0, "Big", 300}}));
    const SectionPlan plan =
        plan_section_edit(h.current(), {big, 300, SectionMode::Copy}, h.current().next_id);
    CHECK(!plan.ok());
    CHECK(plan.compound.ops.empty());
    CHECK(mentions(plan.refusal, "301 operations"));
    CHECK(mentions(plan.refusal, "256"));
    return 0;
}

int test_sections_list() {
    Fixture f;
    if (build(f) != 0) return 1;
    const std::vector<Marker> list = section_markers(f.h.current());
    CHECK(list.size() == 1 && list[0].id == f.verse);
    return 0;
}

}  // namespace

int main() {
    int failures = 0;
    failures += test_copy_before();
    failures += test_copy_after_splits_destination();
    failures += test_move_later();
    failures += test_move_earlier_straddling();
    failures += test_markers_only_section();
    failures += test_refusals();
    failures += test_over_cap();
    failures += test_sections_list();
    if (failures != 0) {
        std::fprintf(stderr, "%d arrangement test(s) failed\n", failures);
        return 1;
    }
    std::printf("arrangement tests passed\n");
    return 0;
}
