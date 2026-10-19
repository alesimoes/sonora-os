// SPDX-License-Identifier: GPL-3.0-or-later
//
// The handlers install_session_commands() puts on a real Session. Until M4
// these lived in ui/keymap's test, which therefore linked engine/control;
// the handlers are engine/control's, so their test is too.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

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

// --- with a punch range (PunchFor) ------------------------------------------

constexpr uint64_t kA = 3000, kB = 5000, kH = 1000;

sonora::PunchRange fixed_punch(uint64_t b = kB) {
    sonora::PunchRange r;
    r.capture_from = kA - kH;
    r.capture_to = b + kH;
    r.monitor_in = kA;
    r.monitor_out = b;
    r.crossfade = 40;
    r.shape = sonora::FadeCurve::EqualPower;
    return r;
}

sonora::SessionConfig punch_config(const Dir& dir) {
    sonora::SessionConfig cfg;
    cfg.dir = dir.path.string();
    cfg.track_count = 1;
    cfg.sample_rate = 1000;
    cfg.ring_seconds = 20.0;
    return cfg;
}

void roll_to(sonora::Session& s, uint64_t to) {
    float block[64];
    for (float& f : block) f = 0.5f;
    const float* inputs[1] = {block};
    sonora::backend::AudioBlock b{};
    b.inputs = inputs;
    b.input_count = 1;
    b.nframes = 64;
    while (s.transport().playhead() < to) s.process(b);
}

// Rolling at or past B, a punch would capture only handle and place
// nothing: a take shown as Recording that is then lost. Refused instead,
// with the reason, the transport rolling on and no file opened, at B
// exactly and inside [B, B + H) as past B + H already was.
int test_record_rolling_past_the_punch_out_is_refused() {
    Dir dir("sonora_test_session_commands_punch_late");
    const sonora::SessionConfig cfg = punch_config(dir);
    const fs::path wav = dir.path / "Audio Files" / "Audio 1_01.wav";
    {
        sonora::Session s(cfg);
        CHECK(s.create_dirs());
        CommandRegistry reg;
        int take = 1;
        bool quit = false;
        uint64_t b = kB;
        sonora::install_session_commands(reg, s, take, quit, nullptr, nullptr,
                                         [&b](sonora::PunchRange& r) {
                                             r = fixed_punch(b);
                                             return true;
                                         });
        s.tracks()[0]->set_rec_armed(true);
        s.disk().start();
        s.play();
        roll_to(s, kB + 100);
        const uint64_t playhead = s.transport().playhead();
        CHECK(playhead > kB && playhead < kB + kH);

        for (const char* id : {"transport.record", "transport.toggle_record"}) {
            for (const uint64_t out : {kB, playhead}) {
                b = out;
                std::string why;
                CHECK(reg.dispatch(id, CommandContext{}, why) == DispatchResult::Failed);
                if (!mentions(why, "before the playhead")) {
                    std::fprintf(stderr, "  %s: %s\n", id, why.c_str());
                    return 1;
                }
                CHECK(s.transport().state() == sonora::TransportState::Rolling);
                CHECK(!fs::exists(wav));
            }
        }
        // Still before B: the punch starts where the playhead is.
        b = playhead + 64;
        CHECK(reg.dispatch("transport.record") == DispatchResult::Ok);
        CHECK(s.transport().state() == sonora::TransportState::Recording);
        CHECK(s.stop());
    }
    return 0;
}

// Space-bar recording keys reach the punch too: toggle_record with a range
// set starts a punch over [A - H, B + H), and its second press stops it.
int test_toggle_record_with_a_punch_range_punches() {
    Dir dir("sonora_test_session_commands_punch_toggle");
    const sonora::SessionConfig cfg = punch_config(dir);
    {
        sonora::Session s(cfg);
        CHECK(s.create_dirs());
        CommandRegistry reg;
        int take = 1;
        bool quit = false;
        sonora::install_session_commands(reg, s, take, quit, nullptr, nullptr,
                                         [](sonora::PunchRange& r) {
                                             r = fixed_punch();
                                             return true;
                                         });
        s.tracks()[0]->set_rec_armed(true);
        s.disk().start();
        CHECK(reg.dispatch("transport.toggle_record") == DispatchResult::Ok);
        CHECK(s.transport().state() == sonora::TransportState::Recording);
        roll_to(s, kB + kH + 64);
        CHECK(reg.dispatch("transport.toggle_record") == DispatchResult::Ok);
        CHECK(s.transport().state() == sonora::TransportState::Stopped);
        CHECK(take == 2);
        const auto closed = s.closed_takes();
        CHECK(closed.size() == 1 && closed[0].punched);
        CHECK(closed[0].punch.monitor_in == kA && closed[0].punch.monitor_out == kB);
        CHECK(closed[0].files.size() == 1);
        CHECK(closed[0].files[0].frames == (kB + kH) - (kA - kH));
    }
    return 0;
}

// transport.punch on the fly: rolling, it punches in at the playhead even
// where the session's range would refuse a record (past B); recording, it
// punches out and the transport rolls on, the take closed in band and the
// next number taken; nothing armed is refused by name with the roll left
// alone. Stopped, it records as transport.record does, the range included.
int test_punch_on_the_fly() {
    Dir dir("sonora_test_session_commands_punch_fly");
    const sonora::SessionConfig cfg = punch_config(dir);
    const fs::path first = dir.path / "Audio Files" / "Audio 1_01.wav";
    const fs::path second = dir.path / "Audio Files" / "Audio 1_02.wav";
    {
        sonora::Session s(cfg);
        CHECK(s.create_dirs());
        CommandRegistry reg;
        int take = 1;
        bool quit = false;
        sonora::install_session_commands(reg, s, take, quit, nullptr, nullptr,
                                         [](sonora::PunchRange& r) {
                                             r = fixed_punch();
                                             return true;
                                         });
        CHECK(reg.implemented("transport.punch"));
        s.disk().start();
        s.play();
        roll_to(s, kB + kH + 64);

        std::string why;
        CHECK(reg.dispatch("transport.punch", {}, why) == DispatchResult::Failed);
        CHECK(mentions(why, "no track is armed"));
        CHECK(s.transport().state() == sonora::TransportState::Rolling);

        s.tracks()[0]->set_rec_armed(true);
        CHECK(reg.dispatch("transport.punch", {}, why) == DispatchResult::Ok);
        CHECK(s.transport().state() == sonora::TransportState::Recording);
        CHECK(take == 1);
        roll_to(s, s.transport().playhead() + 640);
        CHECK(reg.dispatch("transport.punch") == DispatchResult::Ok);
        CHECK(s.transport().state() == sonora::TransportState::Rolling);
        CHECK(take == 2);
        roll_to(s, s.transport().playhead() + 64);
        // Closed by the disk thread, which runs on its own.
        std::vector<sonora::Session::ClosedTake> closed;
        for (int i = 0; i < 2000 && closed.empty(); ++i) {
            closed = s.closed_takes();
            if (closed.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(closed.size() == 1 && closed[0].take == 1 && !closed[0].punched);
        CHECK(closed[0].files.size() == 1 && closed[0].files[0].frames == 640);

        CHECK(reg.dispatch("transport.punch") == DispatchResult::Ok);
        CHECK(s.transport().state() == sonora::TransportState::Recording);
        roll_to(s, s.transport().playhead() + 64);
        CHECK(reg.dispatch("transport.stop") == DispatchResult::Ok);
        CHECK(s.transport().state() == sonora::TransportState::Stopped);
        CHECK(take == 3);
        const auto again = s.closed_takes();
        CHECK(again.size() == 1 && again[0].take == 2 && !again[0].punched);

        // Stopped at 0, before A: a take over the session's range.
        s.transport().set_playhead(0);
        s.disk().start();
        CHECK(reg.dispatch("transport.punch") == DispatchResult::Ok);
        CHECK(s.transport().state() == sonora::TransportState::Recording);
        roll_to(s, kB + kH + 64);
        CHECK(reg.dispatch("transport.stop") == DispatchResult::Ok);
        const auto punched = s.closed_takes();
        CHECK(punched.size() == 1 && punched[0].punched);
        CHECK(punched[0].punch.monitor_in == kA && punched[0].punch.monitor_out == kB);
    }
    CHECK(fs::exists(first) && fs::exists(second));
    return 0;
}

}  // namespace

int main() {
    int failures = 0;
    failures += test_session_commands();
    failures += test_arming_locked_during_take();
    failures += test_record_rolling_past_the_punch_out_is_refused();
    failures += test_toggle_record_with_a_punch_range_punches();
    failures += test_punch_on_the_fly();
    if (failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    std::printf("session commands: all tests passed\n");
    return 0;
}
