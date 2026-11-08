// SPDX-License-Identifier: GPL-3.0-or-later
//
// Export Stems, the engine's half (ADR-0085): which stems a request makes,
// what their files are called, what is refused and why, and that a cancelled
// or failed export leaves no folder. The passes are a fake renderer here, so
// nothing plays; the mix itself is test_stems_sum's.
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sonora/command.h"
#include "sonora/stems.h"
#include "sonora/stems_job.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using namespace sonora;
namespace fs = std::filesystem;

constexpr model::Id kBus = 900;

struct TrackSpec {
    model::Id id;
    const char* name;
    model::Id output = model::kMasterOutput;
    bool mute = false;
    bool active_insert = false;
    model::TrackKind kind = model::TrackKind::Audio;
};

// A snapshot by value, no document: each audio track has one region of
// `length` frames from 0, so the default range is [0, length).
model::Snapshot snapshot_of(const std::vector<TrackSpec>& specs, model::Frames length = 4800) {
    auto edit = std::make_shared<model::EditState>();
    model::Snapshot s;
    s.settings.name = "Song";
    for (const TrackSpec& t : specs) {
        auto e = std::make_shared<model::TrackEdit>();
        e->id = t.id;
        e->name = t.name;
        e->kind = t.kind;
        e->output = t.output;
        auto pl = std::make_shared<model::Playlist>();
        pl->id = t.id + 1000;
        if (t.kind == model::TrackKind::Audio && length > 0) {
            model::Region r;
            r.id = t.id + 2000;
            r.length = length;
            pl->regions.push_back(r);
        }
        e->active_playlist = pl->id;
        e->playlists.push_back(pl);
        if (t.active_insert) {
            model::Insert i;
            i.id = t.id + 3000;
            e->inserts.push_back(i);
        }
        edit->tracks.push_back(e);
        model::TrackMix m;
        m.track = t.id;
        m.mute = t.mute;
        s.mix.tracks.push_back(m);
    }
    model::Bus b;
    b.id = kBus;
    b.name = "Drums";
    edit->buses.push_back(b);
    model::BusMix bm;
    bm.bus = kBus;
    s.mix.buses.push_back(bm);
    s.edit = edit;
    return s;
}

api::StemsOptions options(const std::string& arg) {
    api::StemsOptions o;
    std::string why;
    if (!api::parse_stems_argument(arg, o, why)) std::fprintf(stderr, "parse: %s\n", why.c_str());
    return o;
}

std::string fresh_dir(const std::string& name) {
    fs::remove_all(name);
    fs::create_directories(name);
    return name;
}

std::size_t entries(const std::string& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    std::size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        (void)e;
        ++n;
    }
    return n;
}

// What each pass was handed, and a file where the bounce would write one.
struct FakeRender {
    std::vector<model::Snapshot> snapshots;
    std::vector<api::BounceOptions> passes;
    int fail_at = -1;      // this pass fails
    int cancel_at = -1;    // this pass sets the cancel, then succeeds
    std::atomic<bool>* cancel = nullptr;

    StemRenderer renderer() {
        return [this](const model::Snapshot& s, const std::string& dir,
                      const api::BounceOptions& o, const BounceHooks& h, BounceResult& out,
                      std::string& error) {
            const int k = static_cast<int>(passes.size());
            snapshots.push_back(s);
            passes.push_back(o);
            if (k == fail_at) {
                error = "the disk is full";
                return false;
            }
            const std::string rel = std::string(kBouncedDir) + "/" + o.name + "_01.wav";
            std::FILE* f = std::fopen((dir + "/" + rel).c_str(), "wb");
            if (f == nullptr) {
                error = "cannot write";
                return false;
            }
            std::fputs("RIFF", f);
            std::fclose(f);
            if (h.progress) h.progress(1.0);
            if (k == cancel_at && cancel != nullptr) cancel->store(true);
            out.relative_path = rel;
            out.frames = o.end - o.start;
            return true;
        };
    }
};

const model::TrackMix* mix_of(const model::Snapshot& s, model::Id id) { return s.mix.find(id); }

int by_track_takes_every_unmuted_audio_track_in_order() {
    const model::Snapshot s = snapshot_of({{1, "Kick"}, {2, "Snare", model::kMasterOutput, true},
                                           {3, "Bass"},
                                           {4, "Keys", model::kMasterOutput, false, false,
                                            model::TrackKind::Midi}});
    StemsPlan p;
    std::string why;
    CHECK(stems_plan(s, options("by=track name=Mix"), p, why));
    CHECK(p.folder == "Bounced Files/Mix Stems");
    CHECK(p.stems.size() == 2);
    CHECK(p.stems[0].name == "Kick" && p.stems[0].tracks == std::vector<model::Id>{1});
    CHECK(p.stems[1].name == "Bass" && p.stems[1].tracks == std::vector<model::Id>{3});

    // Named tracks come in the order named; the folder defaults to the
    // session's name.
    CHECK(stems_plan(s, options("tracks=3,1"), p, why));
    CHECK(p.folder == "Bounced Files/Song Stems");
    CHECK(p.stems.size() == 2 && p.stems[0].name == "Bass" && p.stems[1].name == "Kick");

    // A named track that is muted, unknown or has no audio is refused.
    CHECK(!stems_plan(s, options("tracks=2"), p, why) && why.find("muted") != std::string::npos);
    CHECK(!stems_plan(s, options("tracks=77"), p, why) &&
          why.find("not an audio track") != std::string::npos);
    CHECK(!stems_plan(s, options("tracks=4"), p, why) &&
          why.find("not an audio track") != std::string::npos);

    // Nothing unmuted: no stems.
    const model::Snapshot quiet = snapshot_of({{1, "Kick", model::kMasterOutput, true}});
    CHECK(!stems_plan(quiet, options(""), p, why) && why.find("no stems") != std::string::npos);
    return 0;
}

int by_output_groups_tracks_by_where_they_feed() {
    const model::Snapshot s = snapshot_of({{1, "Kick", kBus}, {2, "Vox"}, {3, "Snare", kBus},
                                           {4, "Gtr", model::kMasterOutput, true},
                                           {5, "Tom", 555}});
    StemsPlan p;
    std::string why;
    CHECK(stems_plan(s, options("by=output name=Mix"), p, why));
    CHECK(p.stems.size() == 3);
    CHECK(p.stems[0].name == "Drums" && (p.stems[0].tracks == std::vector<model::Id>{1, 3}));
    CHECK(p.stems[1].name == "Master" && p.stems[1].tracks == std::vector<model::Id>{2});
    CHECK(p.stems[2].name == "Output 555" && p.stems[2].tracks == std::vector<model::Id>{5});
    return 0;
}

int a_stem_pass_solos_only_its_tracks() {
    model::Snapshot s = snapshot_of({{1, "Kick", kBus}, {2, "Vox"}, {3, "Snare", kBus}});
    // The operator's own solos, solo safe and bus solo are not the stem's.
    for (model::TrackMix& m : s.mix.tracks) {
        if (m.track == 2) m.solo = true;
        if (m.track == 3) m.solo_safe = true;
    }
    s.mix.buses[0].solo = true;
    const model::Snapshot before = s;
    const Stem drums{"Drums", {1, 3}};
    const model::Snapshot pass = stem_snapshot(s, drums);
    CHECK(mix_of(pass, 1)->solo && mix_of(pass, 3)->solo && !mix_of(pass, 2)->solo);
    CHECK(!mix_of(pass, 3)->solo_safe);
    CHECK(!pass.mix.buses[0].solo);
    CHECK(pass.edit == s.edit);   // the edit is shared, not changed
    CHECK(s == before);           // the snapshot it came from is a value

    // A track with no mix entry gets one, soloed.
    model::Snapshot bare = s;
    bare.mix.tracks.clear();
    const model::Snapshot p2 = stem_snapshot(bare, Stem{"Vox", {2}});
    CHECK(mix_of(p2, 2) != nullptr && mix_of(p2, 2)->solo && mix_of(p2, 1) == nullptr);
    return 0;
}

int files_are_numbered_and_named_and_line_up() {
    CHECK(stem_file_stem(0, "Kick") == "01 Kick");
    CHECK(stem_file_stem(9, "a/b:c\\d") == "10 a_b_c_d");
    CHECK(stem_file_stem(2, "") == "03 Track");
    CHECK(stem_file_stem(99, "X") == "100 X");

    // Two tracks of one name: the numbers keep their files apart.
    const std::string dir = fresh_dir("stems_t_names");
    const model::Snapshot s = snapshot_of({{1, "Gtr"}, {2, "Gtr"}, {3, "Bass"}});
    FakeRender fake;
    StemsResult res;
    std::string why;
    double last = 0.0;
    BounceHooks hooks;
    hooks.progress = [&](double f) { last = f; };
    api::StemsOptions o = options("start=100 end=4000 name=Mix/Final");
    CHECK(stems_offline(s, dir, o, hooks, res, why, fake.renderer()));
    CHECK(res.folder == "Bounced Files/Mix_Final Stems");
    CHECK((res.files == std::vector<std::string>{"Bounced Files/Mix_Final Stems/01 Gtr.wav",
                                                 "Bounced Files/Mix_Final Stems/02 Gtr.wav",
                                                 "Bounced Files/Mix_Final Stems/03 Bass.wav"}));
    for (const std::string& f : res.files) CHECK(fs::is_regular_file(dir + "/" + f));
    CHECK(res.frames == 3900);
    CHECK(last == 1.0);
    // Every pass offline over the same range; nothing left beside the folder.
    CHECK(fake.passes.size() == 3);
    for (const api::BounceOptions& p : fake.passes) {
        CHECK(p.mode == api::BounceMode::Offline && p.start == 100 && p.end == 4000);
    }
    CHECK(entries(dir + "/Bounced Files") == 1);

    // The default end is the session's, resolved once for every pass.
    FakeRender whole;
    CHECK(stems_offline(s, dir, options("name=Whole"), hooks, res, why, whole.renderer()));
    for (const api::BounceOptions& p : whole.passes) CHECK(p.start == 0 && p.end == 4800);

    // The same name again is refused, and the first export is untouched.
    FakeRender again;
    CHECK(!stems_offline(s, dir, o, hooks, res, why, again.renderer()));
    CHECK(why.find("already") != std::string::npos && again.passes.empty());
    CHECK(entries(dir + "/Bounced Files/Mix_Final Stems") == 3);
    return 0;
}

int refusals_say_why() {
    const std::string dir = fresh_dir("stems_t_refuse");
    std::string why;
    const model::Snapshot plain = snapshot_of({{1, "Kick"}, {2, "Bass"}});
    CHECK(stems_check(plain, dir, options("name=Ok"), why));

    // An active insert anywhere: the offline pass does not run it.
    const model::Snapshot fx =
        snapshot_of({{1, "Kick"}, {2, "Bass", model::kMasterOutput, false, true}});
    CHECK(!stems_check(fx, dir, options("tracks=1"), why) &&
          why.find("plug-in inserts") != std::string::npos &&
          why.find("freeze") != std::string::npos);

    // An empty range: a start past the session's end (an equal start and end
    // the parser refuses itself), or nothing on the timeline.
    CHECK(!stems_check(plain, dir, options("start=9000"), why) &&
          why.find("range is empty") != std::string::npos);
    const model::Snapshot empty = snapshot_of({{1, "Kick"}}, 0);
    CHECK(!stems_check(empty, dir, options(""), why) &&
          why.find("range is empty") != std::string::npos);

    // A gain per file would stop the stems summing to the mix.
    CHECK(!stems_check(plain, dir, options("normalise=-14"), why) &&
          why.find("normalised") != std::string::npos);
    // The bounce's own pair refusal holds for options not from the parser,
    // which refuses the pair itself.
    api::StemsOptions flac = options("");
    flac.base.file = api::BounceFile::Flac;
    flac.base.format = api::BounceFormat::Float32;
    CHECK(!stems_check(plain, dir, flac, why) && why.find("FLAC") != std::string::npos);
    CHECK(entries(dir) == 0);
    return 0;
}

struct CommandRig {
    CommandRegistry registry;
    StemsJob job;
    BounceJob bounce;
    BounceExportGate gate{bounce};
    model::Snapshot snap;
    std::string dir;
    bool recording = false;
    std::vector<api::BounceState> said;

    explicit CommandRig(StemRenderer r) : job(std::move(r)) {}

    bool install() {
        StemsCommandDeps d;
        d.bounce = &gate;
        d.snapshot = [this]() { return snap; };
        d.session_dir = [this]() { return dir; };
        d.recording = [this]() { return recording; };
        d.announce = [this](api::BounceState st, uint32_t, uint64_t, const std::string&) {
            said.push_back(st);
        };
        return install_stems_commands(registry, job, std::move(d));
    }
    DispatchResult run(const std::string& arg, std::string& why) {
        CommandContext ctx;
        ctx.argument = arg;
        return registry.dispatch("mix.stems", ctx, why);
    }
};

int the_command_refuses_a_take_real_time_and_no_folder() {
    FakeRender fake;
    CommandRig rig(fake.renderer());
    CHECK(rig.install());
    rig.snap = snapshot_of({{1, "Kick"}});
    rig.dir = fresh_dir("stems_t_cmd");
    std::string why;

    rig.recording = true;
    CHECK(rig.run("name=A", why) == DispatchResult::Failed &&
          why.find("stop recording") != std::string::npos);
    rig.recording = false;

    CHECK(rig.run("mode=realtime name=A", why) == DispatchResult::Failed &&
          why.find("offline only") != std::string::npos);
    CHECK(rig.run("by=sideways", why) == DispatchResult::Failed);

    rig.dir.clear();
    CHECK(rig.run("name=A", why) == DispatchResult::Failed &&
          why.find("save it first") != std::string::npos);
    rig.dir = "stems_t_cmd";

    CHECK(rig.said.empty() && !rig.job.active() && entries(rig.dir) == 0);
    return 0;
}

// The job's renderer waits on the test, not on a clock: it says it entered,
// then holds until the cancel arrives.
struct HoldingRender {
    std::atomic<bool> entered{false};
    StemRenderer renderer() {
        return [this](const model::Snapshot&, const std::string& dir, const api::BounceOptions& o,
                      const BounceHooks& h, BounceResult& out, std::string& error) {
            const std::string rel = std::string(kBouncedDir) + "/" + o.name + "_01.wav";
            std::FILE* f = std::fopen((dir + "/" + rel).c_str(), "wb");
            if (f != nullptr) std::fclose(f);
            entered.store(true);
            while (!h.cancel->load()) std::this_thread::yield();
            std::remove((dir + "/" + rel).c_str());
            (void)out;
            error = "bounce was cancelled";
            return false;
        };
    }
};

int a_cancel_through_the_job_leaves_no_folder() {
    HoldingRender hold;
    CommandRig rig(hold.renderer());
    CHECK(rig.install());
    rig.snap = snapshot_of({{1, "Kick"}, {2, "Bass"}});
    rig.dir = fresh_dir("stems_t_job_cancel");
    std::string why;
    CHECK(rig.run("name=Mix", why) == DispatchResult::Ok);
    CHECK(rig.job.active() && rig.gate.exporting() == false && rig.job.exporting());
    // One export at a time.
    CHECK(rig.run("name=Other", why) == DispatchResult::Failed &&
          why.find("already running") != std::string::npos);
    while (!hold.entered.load()) std::this_thread::yield();
    CHECK(fs::is_directory(rig.dir + "/Bounced Files/Mix Stems"));
    rig.job.cancel();
    while (!rig.job.finished()) std::this_thread::yield();
    const StemsAnnounce announce = [&](api::BounceState st, uint32_t, uint64_t,
                                       const std::string&) { rig.said.push_back(st); };
    follow_stems(rig.job, rig.dir, announce);
    CHECK(!rig.job.active());
    CHECK((rig.said == std::vector<api::BounceState>{api::BounceState::Running,
                                                       api::BounceState::Cancelled}));
    CHECK(!fs::exists(rig.dir + "/Bounced Files/Mix Stems"));
    CHECK(entries(rig.dir + "/Bounced Files") == 0);
    return 0;
}

int a_cancel_or_a_failure_between_passes_removes_the_folder() {
    const model::Snapshot s = snapshot_of({{1, "Kick"}, {2, "Snare"}, {3, "Bass"}});
    BounceHooks hooks;
    StemsResult res;
    std::string why;

    // Cancelled after the first file was moved in: it goes with the folder.
    const std::string dir = fresh_dir("stems_t_cancel");
    std::atomic<bool> cancel{false};
    FakeRender c;
    c.cancel = &cancel;
    c.cancel_at = 0;
    hooks.cancel = &cancel;
    CHECK(!stems_offline(s, dir, options("name=Mix"), hooks, res, why, c.renderer()));
    CHECK(why.find("cancelled") != std::string::npos);
    CHECK(c.passes.size() == 1);
    CHECK(!fs::exists(dir + "/Bounced Files/Mix Stems"));
    CHECK(entries(dir + "/Bounced Files") == 0);

    // The second pass fails: the first stem's file and the folder go.
    const std::string dir2 = fresh_dir("stems_t_fail");
    FakeRender f;
    f.fail_at = 1;
    hooks.cancel = nullptr;
    CHECK(!stems_offline(s, dir2, options("name=Mix"), hooks, res, why, f.renderer()));
    CHECK(why.find("02 Snare") != std::string::npos && why.find("disk is full") != std::string::npos);
    CHECK(!fs::exists(dir2 + "/Bounced Files/Mix Stems"));
    CHECK(entries(dir2 + "/Bounced Files") == 0);
    return 0;
}

int an_export_of_a_session_that_moved_is_not_kept() {
    FakeRender fake;
    CommandRig rig(fake.renderer());
    CHECK(rig.install());
    rig.snap = snapshot_of({{1, "Kick"}});
    rig.dir = fresh_dir("stems_t_moved");
    std::string why;
    CHECK(rig.run("name=Mix", why) == DispatchResult::Ok);
    while (!rig.job.finished()) std::this_thread::yield();
    uint64_t files = 0;
    std::string text;
    const StemsAnnounce announce = [&](api::BounceState st, uint32_t, uint64_t n,
                                       const std::string& t) {
        rig.said.push_back(st);
        files = n;
        text = t;
    };
    follow_stems(rig.job, "elsewhere", announce);
    CHECK(rig.said.back() == api::BounceState::Failed && text.find("moved") != std::string::npos);
    CHECK(!fs::exists(rig.dir + "/Bounced Files/Mix Stems"));

    // Done names the folder and the count.
    CHECK(rig.run("name=Mix", why) == DispatchResult::Ok);
    while (!rig.job.finished()) std::this_thread::yield();
    follow_stems(rig.job, rig.dir, announce);
    CHECK(rig.said.back() == api::BounceState::Done && files == 1 &&
          text == "Bounced Files/Mix Stems");
    CHECK(fs::is_regular_file(rig.dir + "/Bounced Files/Mix Stems/01 Kick.wav"));
    return 0;
}

}  // namespace

int main() {
    int failures = 0;
    failures += by_track_takes_every_unmuted_audio_track_in_order();
    failures += by_output_groups_tracks_by_where_they_feed();
    failures += a_stem_pass_solos_only_its_tracks();
    failures += files_are_numbered_and_named_and_line_up();
    failures += refusals_say_why();
    failures += the_command_refuses_a_take_real_time_and_no_folder();
    failures += a_cancel_through_the_job_leaves_no_folder();
    failures += a_cancel_or_a_failure_between_passes_removes_the_folder();
    failures += an_export_of_a_session_that_moved_is_not_kept();
    if (failures == 0) std::puts("stems: all passed");
    return failures == 0 ? 0 : 1;
}
