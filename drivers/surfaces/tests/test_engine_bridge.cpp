// SPDX-License-Identifier: GPL-3.0-or-later
// The engine side of the surface: events become the commands and parameter
// changes the UI sends (fader touch brackets an automation gesture), and
// published state becomes feedback, only when it changed.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "fake_midi_port.h"
#include "sonora/surfaces/engine_bridge.h"
#include "sonora/surfaces/hui.h"

using namespace sonora::surfaces;
namespace api = sonora::api;

namespace {
int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

class RecordingEngine : public api::EngineControl {
public:
    bool connect(std::string&) override { return true; }
    const api::Hello& hello() const override { return hello_; }
    uint64_t send(const api::CommandRequest& r) override {
        commands.push_back(r);
        return ++seq_;
    }
    uint64_t send(const api::ParameterChange& c) override {
        changes.push_back(c);
        return ++seq_;
    }
    uint64_t send(const api::PluginParameterChange&) override { return ++seq_; }
    uint64_t submit(const api::ModelRequest&) override { return ++seq_; }
    void poll(api::Inbox&) override {}
    api::StateView& state() override { return view_; }

    std::vector<api::CommandRequest> commands;
    std::vector<api::ParameterChange> changes;

private:
    api::Hello hello_;
    api::StateView view_;
    uint64_t seq_ = 0;
};

SurfaceEvent ev(EventKind k, std::uint8_t strip, bool flag, float value = 0) {
    SurfaceEvent e{};
    e.kind = k;
    e.strip = strip;
    e.flag = flag;
    e.value = value;
    return e;
}

void fader_touch_is_an_automation_gesture() {
    RecordingEngine eng;
    EngineSurfaceBridge bridge(eng);
    SurfaceLink link;
    link.push_event(ev(EventKind::Fader, 2, true, 0.78f));
    link.push_event(ev(EventKind::Fader, 2, true, 0.9f));
    link.push_event(ev(EventKind::Fader, 2, false, 0.9f));
    link.push_event(ev(EventKind::Fader, 2, false, 0.5f));  // no touch sense: Move
    bridge.drain(link);
    CHECK(eng.changes.size() == 4);
    CHECK(eng.changes[0].gesture == api::Gesture::Touch && eng.changes[0].track == 2);
    CHECK(std::fabs(eng.changes[0].value - 1.0f) < 0.01f);  // 0.78 of travel is 0 dB
    CHECK(eng.changes[1].gesture == api::Gesture::Move);
    CHECK(eng.changes[2].gesture == api::Gesture::Release);
    CHECK(eng.changes[3].gesture == api::Gesture::Move);
    CHECK(eng.changes[3].param == api::ParamId::Gain);
}

void taper_round_trips() {
    CHECK(std::fabs(surface_gain_from_position(0.78f) - 1.0f) < 1e-4f);
    CHECK(surface_gain_from_position(0.0f) == 0.0f);
    CHECK(std::fabs(surface_gain_from_position(1.0f) - 3.98f) < 0.05f);  // +12 dB
    for (float p = 0.1f; p < 1.0f; p += 0.1f) {
        CHECK(std::fabs(surface_position_from_gain(surface_gain_from_position(p)) - p) < 1e-3f);
    }
}

void buttons_become_commands() {
    RecordingEngine eng;
    EngineSurfaceBridge bridge(eng);
    SurfaceLink link;
    link.push_event(ev(EventKind::Mute, 3, true));
    link.push_event(ev(EventKind::Mute, 3, true));  // already muted: no second toggle
    link.push_event(ev(EventKind::Solo, 4, true));
    link.push_event(ev(EventKind::Arm, 5, true));
    link.push_event(ev(EventKind::Select, 6, true));
    link.push_event(ev(EventKind::Pan, 1, false, -0.5f));
    SurfaceEvent t{};
    t.kind = EventKind::Transport;
    t.transport = TransportCmd::Play;
    link.push_event(t);
    t.transport = TransportCmd::Forward;
    t.value = 9;  // a jog spin of 9 detents is capped
    link.push_event(t);
    bridge.drain(link);
    std::vector<std::string> ids;
    for (const auto& c : eng.commands) ids.push_back(c.command + ":" + std::to_string(c.target));
    CHECK(ids.size() == 9);  // mute, solo, arm, select, play, and four forward steps
    CHECK(ids[0] == "track.toggle_mute:3");
    CHECK(ids[1] == "track.toggle_solo:4");
    CHECK(ids[2] == "track.toggle_arm:5");
    CHECK(ids[3] == "track.select:6");
    CHECK(ids[4] == "transport.play:-1");
    CHECK(ids[5] == "transport.fast_forward:-1");
    CHECK(eng.changes.size() == 1 && eng.changes[0].param == api::ParamId::Pan &&
          eng.changes[0].value == -0.5f);
}

void state_becomes_feedback_once() {
    RecordingEngine eng;
    EngineSurfaceBridge bridge(eng);
    SurfaceLink link;
    api::StateFrame frame;
    frame.tracks.resize(2);
    frame.tracks[0].gain = 1.0f;
    frame.tracks[0].flags = api::kTrackMuted;
    frame.tracks[1].gain = 0.0f;
    frame.transport.state = api::TransportState::Rolling;
    const EngineSurfaceBridge::PeakFn peak = [](std::uint32_t t) { return t == 0 ? 0.5f : 0.0f; };
    bridge.publish(link, frame, peak, {"Kick"});

    int fader0 = 0, mute0 = 0, meters0 = 0, scribbles = 0, play = 0;
    std::string name1;
    SurfaceFeedback f{};
    while (link.pop_feedback(f)) {
        if (f.kind == FeedbackKind::Fader && f.strip == 0) {
            ++fader0;
            CHECK(std::fabs(f.value - 0.78f) < 1e-3f);
        }
        if (f.kind == FeedbackKind::Mute && f.strip == 0 && f.flag) ++mute0;
        if (f.kind == FeedbackKind::Meter && f.strip == 0) ++meters0;
        if (f.kind == FeedbackKind::Scribble) {
            ++scribbles;
            if (f.strip == 1) name1.assign(f.text, kScribbleChars);
        }
        if (f.kind == FeedbackKind::Transport && f.transport == TransportCmd::Play && f.flag) ++play;
    }
    CHECK(fader0 == 1 && mute0 == 1 && meters0 == 1 && scribbles == 2 && play == 1);
    CHECK(name1 == "Trk 02 ");

    // Nothing changed: nothing is pushed, so a slow surface costs no drops.
    bridge.publish(link, frame, peak, {"Kick"});
    CHECK(!link.pop_feedback(f));

    // A fader being held is not pushed at.
    SurfaceLink l2;
    link.push_event(ev(EventKind::Fader, 0, true, 0.5f));
    bridge.drain(link);
    frame.tracks[0].gain = 2.0f;
    bridge.publish(l2, frame, peak, {"Kick"});
    bool moved = false;
    while (l2.pop_feedback(f)) moved = moved || (f.kind == FeedbackKind::Fader && f.strip == 0);
    CHECK(!moved);
}

void hui_runs_on_the_same_bridge() {
    RecordingEngine eng;
    EngineSurfaceBridge bridge(eng);
    SurfaceLink link;
    FakeMidiPort port;
    HuiProtocol hui(port, {});
    // Touch fader 3, move it, release; then press mute on strip 1.
    const std::uint8_t wire[] = {0xB0, 0x0F, 0x03, 0xB0, 0x2F, 0x40, 0xB0, 0x03, 0x64, 0xB0,
                                 0x23, 0x00, 0xB0, 0x0F, 0x03, 0xB0, 0x2F, 0x00, 0xB0, 0x0F,
                                 0x01, 0xB0, 0x2F, 0x42};
    hui.on_midi(wire, sizeof wire, link);
    bridge.drain(link);
    CHECK(eng.changes.size() == 3);
    CHECK(eng.changes[0].gesture == api::Gesture::Touch && eng.changes[0].track == 3);
    CHECK(eng.changes[1].gesture == api::Gesture::Move);
    CHECK(eng.changes[2].gesture == api::Gesture::Release);
    CHECK(eng.commands.size() == 1 && eng.commands[0].command == "track.toggle_mute");

    // And the engine's state reaches the HUI wire.
    api::StateFrame frame;
    frame.tracks.resize(1);
    frame.tracks[0].gain = 1.0f;
    frame.tracks[0].flags = api::kTrackMuted;
    bridge.publish(link, frame, [](std::uint32_t) { return 0.0f; }, {"Kick"});
    hui.render(link);
    bool led = false;
    for (const auto& m : port.sent) {
        if (m == std::vector<std::uint8_t>{0xB0, 0x0C, 0x00, 0x2C, 0x42}) led = true;
    }
    CHECK(led);
}

}  // namespace

int main() {
    fader_touch_is_an_automation_gesture();
    taper_round_trips();
    buttons_become_commands();
    state_becomes_feedback_once();
    hui_runs_on_the_same_bridge();
    if (failures == 0) std::printf("surface bridge: ok\n");
    return failures == 0 ? 0 : 1;
}
