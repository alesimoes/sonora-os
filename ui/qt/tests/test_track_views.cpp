// SPDX-License-Identifier: GPL-3.0-or-later
//
// Track view contract (ADR-0106): the four commands in the catalogue as
// Interface commands, the height presets, and the per-session store.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

#include "edit_controller.h"
#include "sonora/command.h"
#include "track_view_model.h"
#include "track_views.h"

using namespace sonora;
using namespace sonora::ui;

#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                             \
        }                                                                         \
    } while (0)

static int commands() {
    for (const char* id : {"track.height", "track.hide", "track.show_all", "window.tracks_list"}) {
        const CommandSpec* s = find_command(id);
        CHECK(s != nullptr);
        CHECK(s->domain == CommandDomain::Interface);
        CHECK(!is_engine_command(id));
    }
    CHECK(find_command("track.height")->takes_argument);
    CHECK(find_command("track.height")->target == CommandTarget::Track);
    CHECK(find_command("track.hide")->target == CommandTarget::Track);
    CHECK(find_command("track.show_all")->target == CommandTarget::None);
    return 0;
}

static int presets() {
    TrackHeight h;
    for (const char* n : {"small", "medium", "large", "jumbo"}) {
        CHECK(parse_track_height(n, h));
        CHECK(std::string(track_height_name(h)) == n);
    }
    CHECK(!parse_track_height("huge", h) && !parse_track_height("", h));
    return 0;
}

static int store() {
    FakeTrackViewStore st;
    CHECK(st.get("a", 1).height == TrackHeight::Medium && !st.get("a", 1).hidden);
    st.set("a", 1, {TrackHeight::Jumbo, true});
    st.set("a", 2, {TrackHeight::Small, true});
    st.set("b", 1, {TrackHeight::Large, false});
    CHECK(st.get("a", 1).height == TrackHeight::Jumbo && st.get("a", 1).hidden);
    CHECK(!st.get("b", 1).hidden);
    CHECK(st.show_all("a") == 2);
    CHECK(!st.get("a", 1).hidden && st.get("a", 1).height == TrackHeight::Jumbo);
    CHECK(st.show_all("a") == 0 && st.show_all("none") == 0);
    st.forget("b");
    CHECK(st.get("b", 1).height == TrackHeight::Medium);
    return 0;
}

static std::string temp_file(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / "sonora-track-views-test";
    std::filesystem::create_directories(dir);
    const auto path = dir / name;
    std::filesystem::remove(path);
    return path.string();
}

static int file_store() {
    const std::string path = temp_file("views.json");
    {
        TrackViews a(path);
        a.set("s1", 7, {TrackHeight::Jumbo, true});
        a.set("s2", 7, {TrackHeight::Small, false});
        CHECK(a.save_error().empty());
    }
    TrackViews b(path);
    std::string why;
    CHECK(b.load(why) && why.empty());
    CHECK(b.get("s1", 7).height == TrackHeight::Jumbo && b.get("s1", 7).hidden);
    CHECK(b.get("s2", 7).height == TrackHeight::Small && !b.get("s2", 7).hidden);
    // Per session: another session id sees nothing of it.
    CHECK(b.get("s3", 7).height == TrackHeight::Medium && !b.get("s3", 7).hidden);
    // Back to plain removes the entry rather than storing a default.
    b.set("s2", 7, {});
    CHECK(b.customised("s2") == 0);

    // A missing file is not a fault; a corrupt one is ignored, loudly.
    TrackViews none(temp_file("absent.json"));
    CHECK(none.load(why));
    const std::string bad = temp_file("bad.json");
    std::ofstream(bad) << "{ not json";
    TrackViews c(bad);
    why.clear();
    CHECK(!c.load(why) && !why.empty());
    CHECK(c.get("s1", 7).height == TrackHeight::Medium);
    // A height this build does not know drops that entry only.
    const std::string odd = temp_file("odd.json");
    std::ofstream(odd) << R"({"version":1,"sessions":{"s":{"1":{"height":"huge","hidden":true},)"
                          R"("2":{"height":"large","hidden":false}}}})";
    TrackViews d(odd);
    CHECK(d.load(why));
    CHECK(!d.get("s", 1).hidden && d.get("s", 2).height == TrackHeight::Large);
    return 0;
}

namespace {
struct World {
    std::string session = "one";
    int count = 3;
    std::map<int, bool> armed;
    TrackViewSource source() {
        return {[this] { return session; }, [this] { return count; },
                [](int i) { return std::uint64_t(100 + i); },
                [](int i) { return "Track " + std::to_string(i); },
                [this](int i) { return armed.count(i) != 0 && armed[i]; }};
    }
};
}  // namespace

static int view_model() {
    World w;
    FakeTrackViewStore st;
    TrackViewModel m(st, w.source());
    std::string why;
    CHECK(!m.active());
    CHECK(m.setHeight(1, "large", why) && m.active());
    CHECK(m.row(1, 101).scale > 1.0 && !m.row(1, 101).hidden);
    CHECK(!m.setHeight(1, "huge", why) && !why.empty());
    CHECK(!m.setHeight(9, "large", why));
    CHECK(m.hide(2, why) && m.row(2, 102).hidden);
    // An armed track is never hidden: refused, held, and the row stays.
    w.armed[0] = true;
    why.clear();
    CHECK(!m.hide(0, why) && why.find("armed") != std::string::npos);
    CHECK(!m.setHiddenHeld(0, true) && !m.notice().isEmpty());
    CHECK(!m.row(0, 100).hidden);
    // Even a hide stored while it was disarmed does not hide it once armed.
    w.armed[0] = false;
    CHECK(m.hide(0, why));
    w.armed[0] = true;
    CHECK(!m.row(0, 100).hidden);
    w.armed[0] = false;
    CHECK(m.hiddenCount() == 2);
    CHECK(m.showAll(why) && m.hiddenCount() == 0 && !m.showAll(why));
    CHECK(m.row(1, 101).scale > 1.0);  // heights stay
    // The held notice belongs to the session: a new one starts clean.
    CHECK(!m.notice().isEmpty());
    w.session = "two";
    CHECK(m.notice().isEmpty() && !m.active());
    w.session = "one";
    CHECK(m.active());
    return 0;
}

// Hidden rows take no room and are skipped; heights move the rows below.
namespace {
struct Policy final : RowPolicy {
    std::map<int, RowSpec> rows;
    bool active() const override { return !rows.empty(); }
    RowSpec row(int index, std::uint64_t) const override {
        const auto it = rows.find(index);
        return it == rows.end() ? RowSpec{} : it->second;
    }
};
}  // namespace

static int geometry() {
    using namespace sonora::ui::model;
    ModelDocument doc(make_empty_session(4, 48000));
    EditController ctl(doc);
    ctl.set_viewport(1000, 600);
    const double h = ctl.view().track_height;
    Policy p;
    ctl.set_row_policy(&p);
    CHECK(ctl.view().tops.empty());  // plain until something is sized
    p.rows[0] = {height_scale(TrackHeight::Large), false};
    p.rows[1] = {1.0, true};
    const ViewState& v = ctl.view();
    CHECK(v.row_h(0) > h && v.row_hidden(1) && !v.row_hidden(2));
    CHECK(v.y_of_track(1) == v.row_h(0));
    CHECK(v.y_of_track(2) == v.y_of_track(1));  // the hidden row has no room
    // A pixel under the first row lands on the next visible track, never the hidden one.
    CHECK(v.track_at(v.row_h(0) + 1) == 2);
    CHECK(ctl.hit(50, v.row_h(0) + 1).track == 2);
    // Keyboard navigation steps over it.
    std::string why;
    ctl.select_track(0);
    CHECK(ctl.select_next_track(1, why) && ctl.selection().track == 2);
    CHECK(ctl.select_next_track(-1, why) && ctl.selection().track == 0);
    // Automation lanes of a hidden track take no room either.
    CHECK(ctl.toggle_autolanes(1, why));
    CHECK(ctl.view().y_of_track(2) == ctl.view().y_of_track(1));
    // Plain again: uniform geometry returns.
    p.rows.clear();
    CHECK(ctl.toggle_autolanes(1, why));
    CHECK(ctl.view().tops.empty() && ctl.view().heights.empty());
    return 0;
}

int main() {
    return commands() || presets() || store() || file_store() || view_model() || geometry();
}
