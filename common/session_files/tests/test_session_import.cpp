// SPDX-License-Identifier: GPL-3.0-or-later
//
// Import Session Data's planner (ADR-0123): the plan applied with
// model::apply passes check_invariants and one undo takes it back; ids run
// from the destination's next_id in a fixed order, so the same inputs give
// the same bytes; a file the destination already holds (same umid) is
// reused; a taken name gets a suffix; every refusal names its reason and
// leaves the plan empty; what v1 does not import is listed.
#include <cstdio>
#include <functional>

#include "session_import_fixture.h"
#include "sonora/model/op_codec.h"

namespace sf = sonora::session_files;
using namespace import_fixture;
using Reason = sf::ImportRefusal::Reason;

namespace {

template <typename T>
std::size_t members_of(const Compound& c) {
    std::size_t n = 0;
    for (const Member& m : c.ops) n += m.holds<T>() ? 1 : 0;
    return n;
}

const TrackEdit* track_named(const Snapshot& s, const std::string& name) {
    for (const auto& t : s.edit->tracks) {
        if (t->name == name) return t.get();
    }
    return nullptr;
}

bool any_mentions(const std::vector<std::string>& lines, const char* a, const char* b) {
    for (const std::string& l : lines) {
        if (l.find(a) != std::string::npos && l.find(b) != std::string::npos) return true;
    }
    std::fprintf(stderr, "  no line mentions \"%s\" and \"%s\"\n", a, b);
    return false;
}

struct Fixture {
    SourceSession src;
    Snapshot dest;
    bool ok = build_source(src) && build_dest(dest);
};

// Applies the plan as the interface does after the copies: one operation.
bool apply_plan(const sf::ImportPlan& plan, History& h) {
    std::string error;
    if (!h.apply(Operation(plan.ops), error)) {
        std::fprintf(stderr, "  the plan was refused: %s\n", error.c_str());
        return false;
    }
    return invariants_hold(h.current());
}

int types_default_to_nothing() {
    sf::SessionImportRequest request;
    CHECK(request.tracks.empty());
    CHECK(!request.markers);       // another song's markers only on request
    CHECK(request.offset == 0);
    CHECK(!request.take_open);
    CHECK(!request.source_readable);
    sf::ImportPlan plan;
    CHECK(plan.ops.ops.empty());
    CHECK(plan.copies.empty() && plan.dropped.empty() && plan.approximated.empty());
    sf::ImportRefusal refusal;
    CHECK(refusal.reason == Reason::None);
    sf::ImportableTrack row;
    CHECK(row.id == 0 && row.importable && row.why.empty());
    return 0;
}

int lists_the_source_tracks() {
    Fixture f;
    CHECK(f.ok);
    CHECK(sf::list_importable_tracks(new_session(settings())).empty());
    const std::vector<sf::ImportableTrack> rows = sf::list_importable_tracks(f.src.snapshot);
    CHECK(rows.size() == 3);
    CHECK(rows[0].id == f.src.kick && rows[0].name == "Kick" && rows[0].importable);
    CHECK(rows[0].regions == 2 && rows[0].length == 96000 && rows[0].channels == 1);
    CHECK(rows[1].name == "Gtr" && rows[1].channels == 2 && rows[1].length == 192000);
    CHECK(rows[2].id == f.src.keys && !rows[2].importable && mentions(rows[2].why, "MIDI"));
    return 0;
}

int imports_two_of_three_as_one_undo_step() {
    Fixture f;
    CHECK(f.ok);
    sf::ImportPlan plan;
    sf::ImportRefusal refusal;
    CHECK(sf::plan_session_import(f.dest, f.src.snapshot, request({f.src.kick, f.src.gtr}), plan,
                                  refusal));
    CHECK(refusal.reason == Reason::None);
    CHECK(plan.ops.label == "Import Session Data");
    CHECK(plan.ops.ops.size() <= kMaxCompoundMembers);
    History h(f.dest);
    CHECK(apply_plan(plan, h));
    const Snapshot& after = h.current();
    CHECK(after.edit->tracks.size() == 3);
    const TrackEdit* kick = track_named(after, "Kick 2");
    const TrackEdit* gtr = track_named(after, "Gtr");
    CHECK(kick != nullptr && gtr != nullptr);
    const Playlist& kp = playlist_of(after, 1);
    CHECK(kp.regions.size() == 2 && kp.crossfades.size() == 1);
    CHECK(kp.regions[0].fade_in.length == 480);
    const TrackMix* kmix = after.mix.find(kick->id);
    CHECK(kmix != nullptr && !kmix->solo && kmix->gain_db == -3.0f);
    CHECK(gtr->output == kMasterOutput && gtr->sends.empty());
    CHECK(gtr->inserts.size() == 1 && gtr->inserts[0].slot == 2);
    const InsertMix* eq = after.mix.find_insert(gtr->inserts[0].id);
    CHECK(eq != nullptr && eq->params.size() == 1 && !eq->state);
    CHECK(gtr->lane(AutomationParam::Volume) != nullptr);
    CHECK(gtr->colour == f.src.snapshot.edit->tracks[1]->colour);
    CHECK(after.edit->groups.empty());
    CHECK(h.undo_depth() == 1);   // one Compound, one step
    CHECK(h.undo());
    CHECK(*h.current().edit == *f.dest.edit);
    CHECK(invariants_hold(h.current()));
    return 0;
}

// Every id the plan introduces, in member order.
std::vector<Id> ids_of(const Compound& c) {
    std::vector<Id> ids;
    for (const Member& m : c.ops) {
        if (const auto* s = m.get_if<AddSource>()) ids.push_back(s->source.id);
        if (const auto* t = m.get_if<AddTrack>()) {
            ids.push_back(t->track);
            ids.push_back(t->playlist);
        }
        if (const auto* r = m.get_if<PlaceRegion>()) ids.push_back(r->region.id);
        if (const auto* i = m.get_if<AddInsert>()) ids.push_back(i->insert);
        if (const auto* k = m.get_if<AddMarker>()) ids.push_back(k->marker.id);
    }
    return ids;
}

int ids_run_from_next_id_in_a_fixed_order() {
    Fixture f;
    CHECK(f.ok);
    sf::SessionImportRequest r = request({f.src.gtr, f.src.kick});
    r.markers = true;
    sf::ImportPlan a, b;
    sf::ImportRefusal refusal;
    CHECK(sf::plan_session_import(f.dest, f.src.snapshot, r, a, refusal));
    CHECK(sf::plan_session_import(f.dest, f.src.snapshot, r, b, refusal));
    std::string bytes_a, bytes_b, error;
    CHECK(encode_operation(Operation(a.ops), bytes_a, error));
    CHECK(encode_operation(Operation(b.ops), bytes_b, error));
    CHECK(!bytes_a.empty() && bytes_a == bytes_b);
    // Sources first, then each track in request order, then the markers;
    // every id handed out is used, so they are contiguous.
    CHECK(a.ops.ops.front().holds<AddSource>());
    CHECK(a.ops.ops.back().holds<AddMarker>());
    const std::vector<Id> ids = ids_of(a.ops);
    for (std::size_t i = 0; i < ids.size(); ++i) CHECK(ids[i] == f.dest.next_id + i);
    std::size_t first_track = 0;
    while (!a.ops.ops[first_track].holds<AddTrack>()) ++first_track;
    CHECK(get<AddTrack>(a.ops.ops[first_track]).name == "Gtr");
    History h(f.dest);
    CHECK(apply_plan(a, h));
    CHECK(h.current().next_id == f.dest.next_id + ids.size());
    return 0;
}

int a_shared_umid_is_reused_and_a_taken_name_suffixed() {
    Fixture f;
    CHECK(f.ok);
    sf::ImportPlan plan;
    sf::ImportRefusal refusal;
    CHECK(sf::plan_session_import(f.dest, f.src.snapshot, request({f.src.kick, f.src.gtr}), plan,
                                  refusal));
    CHECK(members_of<AddSource>(plan.ops) == 2);
    CHECK(plan.copies.size() == 2);
    CHECK(plan.copies[0].source_path == "/sessions/Source/Audio Files/Kick_02.wav");
    CHECK(plan.copies[0].dest_path == "Audio Files/Kick_02.wav");
    CHECK(plan.copies[1].dest_path == "Audio Files/Gtr_01 2.wav");
    CHECK(plan.copies[0].source == get<AddSource>(plan.ops.ops[0]).source.id);
    CHECK(any_mentions(plan.approximated, "Gtr_01.wav", "Gtr_01 2.wav"));
    CHECK(any_mentions(plan.approximated, "\"Kick\"", "\"Kick 2\""));
    Id dest_kick_file = 0;
    for (const Source& s : f.dest.sources->sources) {
        if (s.umid == kKickUmid) dest_kick_file = s.id;
    }
    History h(f.dest);
    CHECK(apply_plan(plan, h));
    const Playlist& kp = playlist_of(h.current(), 1);
    CHECK(kp.regions[0].channels[0].source == dest_kick_file);
    const Source* copied = h.current().sources->find(plan.copies[0].source);
    CHECK(copied != nullptr && copied->kind == SourceKind::Imported && copied->take == 0);
    return 0;
}

bool refused(const Snapshot& dest, const Snapshot& src, const sf::SessionImportRequest& r,
             Reason reason, const char* says) {
    sf::ImportPlan plan;
    plan.dropped.push_back("left over from a previous call");
    sf::ImportRefusal refusal;
    if (sf::plan_session_import(dest, src, r, plan, refusal)) {
        std::fprintf(stderr, "  planned where \"%s\" was expected\n", says);
        return false;
    }
    if (refusal.reason != reason) {
        std::fprintf(stderr, "  refused for another reason: %s\n", refusal.message.c_str());
        return false;
    }
    return mentions(refusal.message, says) && plan.ops.ops.empty() && plan.copies.empty() &&
           plan.dropped.empty() && plan.approximated.empty();
}

int every_refusal_names_its_reason() {
    Fixture f;
    CHECK(f.ok);
    const Snapshot& s = f.src.snapshot;
    CHECK(refused(f.dest, s, request({}), Reason::InvalidRequest, "no track"));
    CHECK(refused(f.dest, s, request({9999}), Reason::InvalidRequest, "9999"));
    CHECK(refused(f.dest, s, request({f.src.kick, f.src.kick}), Reason::InvalidRequest, "twice"));
    sf::SessionImportRequest r = request({f.src.kick});
    r.source_folder.clear();
    CHECK(refused(f.dest, s, r, Reason::InvalidRequest, "folder"));
    r = request({f.src.kick});
    r.source_format_version = 0;
    CHECK(refused(f.dest, s, r, Reason::InvalidRequest, "format version"));
    r = request({f.src.keys});
    CHECK(refused(f.dest, s, r, Reason::InvalidRequest, "nothing to import"));
    r = request({f.src.kick});
    r.take_open = true;
    CHECK(refused(f.dest, s, r, Reason::TakeOpen, "take"));
    r = request({f.src.kick});
    r.source_format_version = kFormatVersion + 1;
    CHECK(refused(f.dest, s, r, Reason::NewerFormat, "newer Sonora"));
    SessionSettings at_44 = settings();
    at_44.sample_rate = 44100;
    CHECK(refused(new_session(at_44), s, request({f.src.kick}), Reason::SampleRate, "44100"));
    r = request({f.src.kick, f.src.gtr});
    r.source_readable = [](const std::string& p) { return p.find("Kick_02") == std::string::npos; };
    CHECK(refused(f.dest, s, r, Reason::MissingFile, "Kick_02.wav"));
    sf::ImportPlan plan;
    sf::ImportRefusal refusal;
    CHECK(!sf::plan_session_import(f.dest, s, r, plan, refusal));
    CHECK(refusal.files == std::vector<std::string>{"Audio Files/Kick_02.wav"});
    return 0;
}

// Three tracks of 100 regions each on one file: 1 + 3 x 101 members.
bool build_large(Snapshot& out, std::vector<Id>& tracks) {
    History h(new_session(settings()));
    Ids ids(h.current());
    const AddSource take = file(ids, "Long.wav", 1, "");
    FIXTURE_APPLY(h, take);
    for (int t = 0; t < 3; ++t) {
        const AddTrack add = add_track(ids, "T" + std::to_string(t));
        FIXTURE_APPLY(h, add);
        tracks.push_back(add.track);
        for (int i = 0; i < 100; ++i) {
            const Region r = region(ids(), take.source.id, i * 1000, 1000);
            FIXTURE_APPLY(h, (PlaceRegion{add.playlist, r, 0}));
        }
    }
    out = h.current();
    return true;
}

int past_the_member_limit_is_refused_naming_what_fits() {
    Snapshot large;
    std::vector<Id> tracks;
    CHECK(build_large(large, tracks));
    const Snapshot dest = new_session(settings());
    CHECK(refused(dest, large, request(tracks), Reason::TooLarge, "the first 2 fit"));
    sf::ImportPlan plan;
    sf::ImportRefusal refusal;
    CHECK(sf::plan_session_import(dest, large, request({tracks[0], tracks[1]}), plan, refusal));
    History h(dest);
    CHECK(apply_plan(plan, h));
    return 0;
}

int what_is_not_imported_is_listed() {
    Fixture f;
    CHECK(f.ok);
    sf::ImportPlan plan;
    sf::ImportRefusal refusal;
    CHECK(sf::plan_session_import(f.dest, f.src.snapshot,
                                  request({f.src.kick, f.src.gtr, f.src.keys}), plan, refusal));
    CHECK(any_mentions(plan.dropped, "\"Keys\"", "MIDI"));
    CHECK(any_mentions(plan.dropped, "\"Gtr\"", "\"Verb\""));
    CHECK(any_mentions(plan.dropped, "\"Kick 2\"", "\"Rhythm\""));
    CHECK(any_mentions(plan.approximated, "\"Sub\"", "Master"));
    CHECK(any_mentions(plan.approximated, "\"Test EQ\"", "state"));
    CHECK(members_of<AddTrack>(plan.ops) == 2);
    History h(f.dest);
    CHECK(apply_plan(plan, h));
    return 0;
}

int the_switches_choose_what_comes() {
    Fixture f;
    CHECK(f.ok);
    sf::SessionImportRequest r = request({f.src.kick, f.src.gtr});
    r.material = false;
    r.mix = false;
    r.inserts = false;
    r.automation = false;
    sf::ImportPlan plan;
    sf::ImportRefusal refusal;
    CHECK(sf::plan_session_import(f.dest, f.src.snapshot, r, plan, refusal));
    CHECK(plan.copies.empty() && members_of<AddSource>(plan.ops) == 0);
    CHECK(members_of<PlaceRegion>(plan.ops) == 0 && members_of<AddInsert>(plan.ops) == 0);
    CHECK(members_of<EditAutomation>(plan.ops) == 0 && members_of<AddMarker>(plan.ops) == 0);
    History h(f.dest);
    CHECK(apply_plan(plan, h));
    const TrackMix* kmix = h.current().mix.find(track_named(h.current(), "Kick 2")->id);
    CHECK(kmix != nullptr && kmix->gain_db == 0.0f && !kmix->solo);

    r = request({f.src.kick, f.src.gtr});
    r.markers = true;
    r.offset = 1000;
    CHECK(sf::plan_session_import(f.dest, f.src.snapshot, r, plan, refusal));
    History h2(f.dest);
    CHECK(apply_plan(plan, h2));
    const Snapshot& after = h2.current();
    CHECK(playlist_of(after, 1).regions[0].position == 1000);
    CHECK(playlist_of(after, 2).regions[0].position == 97000);
    CHECK(after.edit->markers->markers.size() == 1);
    CHECK(after.edit->markers->markers[0].position == 2000);
    CHECK(track_named(after, "Gtr")->lane(AutomationParam::Volume)->points[0].at == 1000);
    return 0;
}

}  // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<int()>>> tests = {
        {"types_default_to_nothing", types_default_to_nothing},
        {"lists_the_source_tracks", lists_the_source_tracks},
        {"imports_two_of_three_as_one_undo_step", imports_two_of_three_as_one_undo_step},
        {"ids_run_from_next_id_in_a_fixed_order", ids_run_from_next_id_in_a_fixed_order},
        {"a_shared_umid_is_reused_and_a_taken_name_suffixed",
         a_shared_umid_is_reused_and_a_taken_name_suffixed},
        {"every_refusal_names_its_reason", every_refusal_names_its_reason},
        {"past_the_member_limit_is_refused_naming_what_fits",
         past_the_member_limit_is_refused_naming_what_fits},
        {"what_is_not_imported_is_listed", what_is_not_imported_is_listed},
        {"the_switches_choose_what_comes", the_switches_choose_what_comes},
    };
    int failed = 0;
    for (const auto& t : tests) {
        if (t.second() != 0) {
            std::fprintf(stderr, "FAILED: %s\n", t.first);
            ++failed;
        }
    }
    if (failed != 0) return 1;
    std::printf("session_import: %zu tests ok\n", tests.size());
    return 0;
}
