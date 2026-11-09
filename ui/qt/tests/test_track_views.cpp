// SPDX-License-Identifier: GPL-3.0-or-later
//
// Track view contract (ADR-0106): the four commands in the catalogue as
// Interface commands, the height presets, and the per-session store.
#include <cstdio>

#include "sonora/command.h"
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

int main() {
    return commands() || presets() || store();
}
