// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cross-session paste's planner (ADR-0142): a source the destination already
// holds is reused (same id and content, or same non-empty umid); one it lacks
// becomes an AddSource with an id from next_id upward and a file copy whose
// name never collides; a missing origin file, an unknown origin folder or a
// different rate refuses the paste and leaves the plan empty; the AddSources
// are handed over only when every copy succeeded; and Undo of the paste
// removes its region, never the audio.
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <system_error>

#include "model_test.h"
#include "sonora/session_files/paste_sources.h"
#include "sonora/session_files/session_folder.h"

namespace sf = sonora::session_files;
using namespace model_test;

namespace {

const char* kUmid = "3333333333333333333333333333333333333333333333333333333333333333";

// A destination with one track and two takes in its pool: take_1.wav (id 3,
// umid kUmid) and take_2.wav (id 4, no umid).
struct Dest {
    History h{new_session(settings())};
    Id track = 0, playlist = 0, take1 = 0, take2 = 0;
    bool ok = build();

    bool build() {
        Ids ids(h.current());
        const AddTrack t = add_track(ids, "Vox");
        AddSource a = add_source(ids, 480000);
        a.source.umid = kUmid;
        const AddSource b = add_source(ids, 480000);
        std::string e;
        if (!h.apply(t, e) || !h.apply(a, e) || !h.apply(b, e)) {
            std::fprintf(stderr, "fixture: %s\n", e.c_str());
            return false;
        }
        track = t.track;
        playlist = t.playlist;
        take1 = a.source.id;
        take2 = b.source.id;
        return true;
    }
    const Snapshot& now() const { return h.current(); }
};

// A source as another session's pool held it.
Source origin_source(Id id, const std::string& name, const std::string& umid = "") {
    Source s;
    s.id = id;
    s.path = "Audio Files/" + name;
    s.channels = 1;
    s.length = 480000;
    s.kind = SourceKind::Recorded;
    s.take = 7;
    s.recorded_on_track = 99;
    s.umid = umid;
    return s;
}

sf::PasteSourcesRequest from_origin(std::vector<Source> sources) {
    sf::PasteSourcesRequest r;
    r.sources = std::move(sources);
    r.origin_folder = "/media/stick/Other Song";
    r.origin_sample_rate = 48000;
    return r;
}

// The AddSource at `i`, or an empty source when the member is something else.
Source added(const sf::PasteSourcesPlan& plan, std::size_t i) {
    const AddSource* a = plan.ops[i].get_if<AddSource>();
    return a != nullptr ? a->source : Source{};
}

bool empty_refusal(const sf::PasteSourcesPlan& plan) {
    return !plan.problem.empty() && plan.remap.empty() && plan.ops.empty() &&
           plan.copies.empty();
}

int a_source_of_this_session_maps_to_itself() {
    Dest d;
    CHECK(d.ok);
    sf::PasteSourcesRequest r;   // no origin folder: none is needed
    r.sources = {*d.now().sources->find(d.take2)};
    sf::PasteSourcesPlan plan;
    CHECK(sf::plan_paste_sources(d.now(), r, plan));
    CHECK(plan.problem.empty());
    CHECK(plan.remap.at(d.take2) == d.take2);
    CHECK(plan.ops.empty() && plan.copies.empty());
    CHECK(plan.next_id == d.now().next_id);
    std::printf("  same session: source %llu stays %llu, nothing to copy\n",
                static_cast<unsigned long long>(d.take2),
                static_cast<unsigned long long>(plan.remap.at(d.take2)));
    return 0;
}

int a_shared_umid_is_reused() {
    Dest d;
    CHECK(d.ok);
    // Same id as take_2 here but other audio: the umid decides, not the id.
    sf::PasteSourcesRequest r = from_origin({origin_source(d.take2, "Lead.wav", kUmid)});
    sf::PasteSourcesPlan plan;
    CHECK(sf::plan_paste_sources(d.now(), r, plan));
    CHECK(plan.remap.at(d.take2) == d.take1);
    CHECK(plan.ops.empty() && plan.copies.empty());
    // A shorter file under the same umid does not hold the clipboard's span.
    Source longer = origin_source(50, "Lead.wav", kUmid);
    longer.length = 480001;
    CHECK(sf::plan_paste_sources(d.now(), from_origin({longer}), plan));
    CHECK(plan.ops.size() == 1 && plan.copies.size() == 1);
    std::printf("  umid reuse: origin %llu -> %llu here, no copy\n",
                static_cast<unsigned long long>(d.take2),
                static_cast<unsigned long long>(d.take1));
    return 0;
}

int new_sources_take_ids_from_next_id() {
    Dest d;
    CHECK(d.ok);
    const Id first = d.now().next_id;
    // 50 twice (two lanes name it) and 1, which this session uses for
    // something else.
    sf::PasteSourcesRequest r = from_origin({origin_source(50, "Gtr.wav"),
                                             origin_source(50, "Gtr.wav"),
                                             origin_source(1, "Bass.wav")});
    sf::PasteSourcesPlan plan;
    CHECK(sf::plan_paste_sources(d.now(), r, plan));
    CHECK(plan.ops.size() == 2 && plan.copies.size() == 2);
    CHECK(plan.remap.size() == 2);
    CHECK(plan.remap.at(50) == first && plan.remap.at(1) == first + 1);
    CHECK(plan.next_id == first + 2);
    const Source gtr = added(plan, 0);
    CHECK(gtr.id == first && gtr.path == "Audio Files/Gtr.wav");
    CHECK(gtr.kind == SourceKind::Imported && gtr.take == 0 && gtr.recorded_on_track == 0);
    CHECK(plan.copies[0].source_path == "/media/stick/Other Song/Audio Files/Gtr.wav");
    CHECK(plan.copies[0].dest_path == "Audio Files/Gtr.wav");
    CHECK(plan.copies[0].source == first);
    CHECK(plan.copies[1].source == first + 1);

    // The paste as the interface sends it: the AddSources, then a region on
    // the remapped id, one Compound. Undo removes the region, not the audio.
    Compound paste;
    paste.label = "Paste";
    std::string error;
    CHECK(sf::bring_in_sources(
        plan, [](const std::vector<sf::FileCopy>&, std::string&) { return true; }, paste.ops,
        error));
    PlaceRegion place;
    place.playlist = d.playlist;
    place.region = region(plan.next_id, plan.remap.at(50), 0, 48000);
    paste.ops.push_back(place);
    CHECK_APPLY(d.h, Operation(paste));
    CHECK(invariants_hold(d.now()));
    CHECK(playlist_of(d.now(), 0).regions.size() == 1);
    CHECK(d.h.undo());
    CHECK(playlist_of(d.now(), 0).regions.empty());
    CHECK(d.now().sources->find(first) != nullptr);
    CHECK(d.now().sources->find(first + 1) != nullptr);
    std::printf("  id remap: 50 -> %llu, 1 -> %llu, paste starts at %llu; undo keeps the audio\n",
                static_cast<unsigned long long>(first),
                static_cast<unsigned long long>(first + 1),
                static_cast<unsigned long long>(plan.next_id));
    return 0;
}

int a_taken_name_gets_a_suffix() {
    Dest d;
    CHECK(d.ok);
    const std::string taken = d.now().sources->find(d.take1)->path;   // ".../take_N.wav"
    const std::string base = taken.substr(std::string("Audio Files/").size());
    std::string upper = base;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    // Two origin files: one named as take_N here, one differing only in case
    // (one file on the stick's exFAT).
    sf::PasteSourcesRequest r =
        from_origin({origin_source(60, base), origin_source(61, upper)});
    sf::PasteSourcesPlan plan;
    CHECK(sf::plan_paste_sources(d.now(), r, plan));
    CHECK(plan.copies.size() == 2);
    const std::string stem = taken.substr(0, taken.size() - 4);
    CHECK(plan.copies[0].dest_path == stem + " 2.wav");
    CHECK(added(plan, 0).path == plan.copies[0].dest_path);
    CHECK(plan.copies[1].dest_path != plan.copies[0].dest_path);
    CHECK(plan.copies[1].dest_path.find(" 3.") != std::string::npos);
    std::printf("  name collision: %s -> %s, %s -> %s\n", base.c_str(),
                plan.copies[0].dest_path.c_str(), upper.c_str(),
                plan.copies[1].dest_path.c_str());
    return 0;
}

int a_missing_origin_file_refuses() {
    Dest d;
    CHECK(d.ok);
    sf::PasteSourcesRequest r =
        from_origin({origin_source(50, "Gtr.wav"), origin_source(51, "Gone.wav")});
    r.source_readable = [](const std::string& p) { return p.find("Gone") == std::string::npos; };
    sf::PasteSourcesPlan plan;
    CHECK(!sf::plan_paste_sources(d.now(), r, plan));
    CHECK(empty_refusal(plan));
    CHECK(mentions(plan.problem, "Audio Files/Gone.wav"));
    CHECK(mentions(plan.problem, "Other Song"));
    CHECK(mentions(plan.problem, "nothing was pasted"));
    std::printf("  missing source file: %s\n", plan.problem.c_str());
    return 0;
}

int an_unknown_origin_or_rate_refuses() {
    Dest d;
    CHECK(d.ok);
    sf::PasteSourcesRequest r = from_origin({origin_source(50, "Gtr.wav")});
    r.origin_folder.clear();
    sf::PasteSourcesPlan plan;
    CHECK(!sf::plan_paste_sources(d.now(), r, plan));
    CHECK(empty_refusal(plan));
    CHECK(mentions(plan.problem, "folder is not known"));

    r = from_origin({origin_source(50, "Gtr.wav")});
    r.origin_sample_rate = 44100;
    CHECK(!sf::plan_paste_sources(d.now(), r, plan));
    CHECK(empty_refusal(plan));
    CHECK(mentions(plan.problem, "44100 Hz"));
    CHECK(mentions(plan.problem, "nothing was pasted"));
    return 0;
}

int nothing_is_handed_over_when_a_copy_fails() {
    Dest d;
    CHECK(d.ok);
    sf::PasteSourcesPlan plan;
    CHECK(sf::plan_paste_sources(d.now(), from_origin({origin_source(50, "Gtr.wav")}), plan));
    std::vector<Member> out;
    std::string error;
    int calls = 0;
    const sf::PasteCopyFn failing = [&calls](const std::vector<sf::FileCopy>&, std::string& e) {
        ++calls;
        e = "Paste: cannot read \"Gtr.wav\"; nothing was pasted";
        return false;
    };
    CHECK(!sf::bring_in_sources(plan, failing, out, error));
    CHECK(calls == 1 && out.empty());
    CHECK(mentions(error, "cannot read"));
    std::printf("  copy fails: %zu members handed over (%s)\n", out.size(), error.c_str());

    // Nothing to copy: the copier is not asked.
    sf::PasteSourcesRequest same;
    same.sources = {*d.now().sources->find(d.take1)};
    CHECK(sf::plan_paste_sources(d.now(), same, plan));
    calls = 0;
    CHECK(sf::bring_in_sources(plan, failing, out, error));
    CHECK(calls == 0 && out.empty());
    return 0;
}

#ifdef SONORA_TEST_DISK_COPIER
namespace fs = std::filesystem;

bool write_file(const fs::path& p) {
    std::ofstream f(p, std::ios::binary);
    f << "RIFF";
    return static_cast<bool>(f);
}

// The real copier: a file already on disk under a planned name (not in the
// pool, so the planner could not know) fails the whole copy, and the other
// file is not left behind.
int the_disk_copier_is_all_or_nothing() {
    Dest d;
    CHECK(d.ok);
    std::error_code ec;
    const fs::path root = fs::temp_directory_path() / "sonora_test_paste_sources";
    fs::remove_all(root, ec);
    const fs::path origin = root / "Origin";
    const fs::path here = root / "Here";
    CHECK(fs::create_directories(origin / sf::kAudioDir, ec));
    CHECK(fs::create_directories(here / sf::kAudioDir, ec));
    CHECK(write_file(origin / sf::kAudioDir / "A.wav"));
    CHECK(write_file(origin / sf::kAudioDir / "B.wav"));
    CHECK(write_file(here / sf::kAudioDir / "B.wav"));   // a stray file

    sf::PasteSourcesRequest r = from_origin({origin_source(50, "A.wav"), origin_source(51, "B.wav")});
    r.origin_folder = origin.string();
    sf::PasteSourcesPlan plan;
    CHECK(sf::plan_paste_sources(d.now(), r, plan));
    std::atomic<bool> cancel{false};
    sf::CopyOptions copy_only;
    copy_only.hard_links = false;
    std::vector<Member> out;
    std::string error;
    CHECK(!sf::bring_in_sources(plan, sf::disk_copier(here.string(), cancel, copy_only), out,
                                error));
    CHECK(out.empty());
    CHECK(!fs::exists(here / sf::kAudioDir / "A.wav"));
    CHECK(mentions(error, "Paste"));
    CHECK(mentions(error, "nothing was pasted"));
    std::printf("  disk copy fails: %zu members, A.wav left behind: %s (%s)\n", out.size(),
                fs::exists(here / sf::kAudioDir / "A.wav") ? "yes" : "no", error.c_str());

    fs::remove(here / sf::kAudioDir / "B.wav", ec);
    CHECK(sf::bring_in_sources(plan, sf::disk_copier(here.string(), cancel, copy_only), out,
                               error));
    CHECK(out.size() == 2);
    CHECK(fs::exists(here / sf::kAudioDir / "A.wav") && fs::exists(here / sf::kAudioDir / "B.wav"));
    fs::remove_all(root, ec);
    return 0;
}
#endif

}  // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<int()>>> tests = {
        {"a_source_of_this_session_maps_to_itself", a_source_of_this_session_maps_to_itself},
        {"a_shared_umid_is_reused", a_shared_umid_is_reused},
        {"new_sources_take_ids_from_next_id", new_sources_take_ids_from_next_id},
        {"a_taken_name_gets_a_suffix", a_taken_name_gets_a_suffix},
        {"a_missing_origin_file_refuses", a_missing_origin_file_refuses},
        {"an_unknown_origin_or_rate_refuses", an_unknown_origin_or_rate_refuses},
        {"nothing_is_handed_over_when_a_copy_fails", nothing_is_handed_over_when_a_copy_fails},
#ifdef SONORA_TEST_DISK_COPIER
        {"the_disk_copier_is_all_or_nothing", the_disk_copier_is_all_or_nothing},
#endif
    };
    int failed = 0;
    for (const auto& t : tests) {
        std::printf("%s\n", t.first);
        if (t.second() != 0) {
            std::fprintf(stderr, "FAILED: %s\n", t.first);
            ++failed;
        }
    }
    if (failed != 0) return 1;
    std::printf("paste_sources: %zu tests ok\n", tests.size());
    return 0;
}
