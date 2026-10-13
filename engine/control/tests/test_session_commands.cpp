// SPDX-License-Identifier: GPL-3.0-or-later
//
// The handlers install_session_commands() puts on a real Session. Until M4
// these lived in ui/keymap's test, which therefore linked engine/control;
// the handlers are engine/control's, so their test is too.
#include <cstdio>
#include <filesystem>
#include <string>

#include "sonora/session.h"
#include "sonora/session_commands.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

namespace fs = std::filesystem;
using sonora::CommandContext;
using sonora::CommandRegistry;
using sonora::DispatchResult;

bool mentions(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

struct Dir {
    fs::path path;
    explicit Dir(const char* name) : path(fs::temp_directory_path() / name) {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ~Dir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

int test_session_commands() {
    Dir dir("sonora_test_session_commands");
    sonora::SessionConfig cfg;
    cfg.dir = dir.path.string();
    cfg.track_count = 4;
    cfg.ring_seconds = 0.25;
    const fs::path wav = dir.path / "Audio Files" / "Audio 1_01.wav";
    {
        sonora::Session s(cfg);
        CHECK(s.create_dirs());
        CommandRegistry reg;
        int take = 1;
        bool quit = false;
        sonora::install_session_commands(reg, s, take, quit);

        // No selection exists, so there is nothing for it to act on, and
        // guessing track 1 would be the port-ordering defect again.
        CHECK(reg.dispatch("track.toggle_arm") == DispatchResult::Failed);

        CommandContext first;
        first.target = 0;
        CHECK(reg.dispatch("track.toggle_arm", first) == DispatchResult::Ok);
        CHECK(s.tracks()[0]->rec_armed());

        // Pro Tools' Command+Space and F12, Logic's R: start, never stop.
        s.disk().start();
        CHECK(reg.implemented("transport.record"));
        CHECK(reg.dispatch("transport.record") == DispatchResult::Ok);
        CHECK(s.transport().state() == sonora::TransportState::Recording);
        CHECK(reg.dispatch("transport.record") == DispatchResult::Ok);
        CHECK(s.transport().state() == sonora::TransportState::Recording);
        CHECK(take == 1);
        CHECK(reg.dispatch("transport.stop") == DispatchResult::Ok);
        CHECK(s.transport().state() == sonora::TransportState::Stopped);
        CHECK(take == 2);

        // Nothing armed means nothing to record, and it says so.
        CHECK(reg.dispatch("track.disarm_all") == DispatchResult::Ok);
        std::string why;
        CHECK(reg.dispatch("transport.record", {}, why) == DispatchResult::Failed);
        CHECK(mentions(why, "no track is armed"));
        CHECK(s.transport().state() == sonora::TransportState::Stopped);

        CHECK(reg.dispatch("app.quit") == DispatchResult::Ok);
        CHECK(quit);
    }
    CHECK(fs::exists(wav));
    return 0;
}

// --- arming is fixed for the length of a take ---
//
// Session::process() writes a track only while it is armed, but its file stays
// open: disarming mid-take and re-arming resumed the file with no gap, so
// that track came out shorter than the rest and out of sync from that point.
// Arming a track with no file open filled a ring nobody drained and counted
// overruns. So every arming command refuses during a take, and says why.
int test_arming_locked_during_take() {
    Dir dir("sonora_test_session_commands_arming");
    sonora::SessionConfig cfg;
    cfg.dir = dir.path.string();
    cfg.track_count = 3;
    cfg.ring_seconds = 0.25;
    sonora::Session s(cfg);
    CHECK(s.create_dirs());
    CommandRegistry reg;
    int take = 1;
    bool quit = false;
    sonora::install_session_commands(reg, s, take, quit);

    CommandContext first;
    first.target = 0;
    CommandContext second;
    second.target = 1;
    CHECK(reg.dispatch("track.toggle_arm", first) == DispatchResult::Ok);
    s.disk().start();
    CHECK(reg.dispatch("transport.record") == DispatchResult::Ok);
    CHECK(s.transport().state() == sonora::TransportState::Recording);

    struct Attempt {
        const char* command;
        CommandContext ctx;
    };
    const Attempt attempts[] = {
        {"track.toggle_arm", first},   // would leave a gap in track 1
        {"track.toggle_arm", second},  // would fill a ring with no file
        {"track.arm_all", {}},
        {"track.disarm_all", {}},
    };
    for (const Attempt& a : attempts) {
        std::string why;
        CHECK(reg.dispatch(a.command, a.ctx, why) == DispatchResult::Failed);
        CHECK(mentions(why, "during a take"));
        CHECK(s.tracks()[0]->rec_armed());
        CHECK(!s.tracks()[1]->rec_armed());
        CHECK(!s.tracks()[2]->rec_armed());
    }

    // Once the take is closed, arming works again.
    CHECK(reg.dispatch("transport.stop") == DispatchResult::Ok);
    CHECK(reg.dispatch("track.toggle_arm", second) == DispatchResult::Ok);
    CHECK(s.tracks()[1]->rec_armed());
    CHECK(reg.dispatch("track.disarm_all") == DispatchResult::Ok);
    CHECK(!s.tracks()[0]->rec_armed());
    return 0;
}

}  // namespace

int main() {
    int failures = 0;
    failures += test_session_commands();
    failures += test_arming_locked_during_take();
    if (failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    std::printf("session commands: all tests passed\n");
    return 0;
}
