// SPDX-License-Identifier: GPL-3.0-or-later
// The MCU protocol against a scripted device: bank to tracks 9-16, the
// motor fader both ways with touch gating, scribble text, transport,
// V-Pot rings, jog, the time display and rate-limited meters.
#include <cmath>
#include <cstdio>
#include <vector>

#include "fake_midi_port.h"
#include "sonora/surfaces/mcu.h"

using namespace sonora::surfaces;

namespace {
int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

using Bytes = std::vector<std::uint8_t>;
using Sent = std::vector<Bytes>;

bool contains(const Sent& s, const Bytes& m) {
    for (const auto& x : s) {
        if (x == m) return true;
    }
    return false;
}

struct Rig {
    FakeMidiPort port;
    SurfaceLink link;
    std::uint64_t now = 1000;
    McuProtocol mcu;
    Rig() : mcu(port, make_config(&now)) {
        mcu.render(link);
        port.take();  // the first render draws the whole page
    }
    static McuProtocol::Config make_config(std::uint64_t* t) {
        McuProtocol::Config c;
        c.clock_ms = [t] { return *t; };
        return c;
    }
    void midi(std::initializer_list<std::uint8_t> b) {
        const Bytes v(b);
        mcu.on_midi(v.data(), v.size(), link);
    }
    bool event(SurfaceEvent& e) { return link.pop_event(e); }
    void tell(FeedbackKind k, std::uint8_t strip, bool flag = false, float value = 0) {
        SurfaceFeedback f{};
        f.kind = k;
        f.strip = strip;
        f.flag = flag;
        f.value = value;
        link.push_feedback(f);
    }
};

void bank_to_tracks_9_to_16() {
    Rig r;
    r.midi({0x90, McuProtocol::kNoteBankRight, 0x7F});
    CHECK(r.mcu.bank_offset() == 8);
    // Fader 1 of the page is track index 8 now.
    r.midi({0xE0, 0x00, 0x40});
    SurfaceEvent e{};
    CHECK(r.event(e));
    CHECK(e.kind == EventKind::Fader && e.strip == 8);
    CHECK(std::fabs(e.value - 8192.0f / 16383.0f) < 1e-4f);
    // Mute on physical strip 3 is track index 10.
    r.midi({0x90, McuProtocol::kNoteMute + 2, 0x7F});
    CHECK(r.event(e));
    CHECK(e.kind == EventKind::Mute && e.strip == 10 && e.flag);
    // The engine's state for track 9 shows on lane 1 after the bank change; a
    // strip outside the page draws nothing.
    r.tell(FeedbackKind::Mute, 9, true);
    r.tell(FeedbackKind::Mute, 1, true);
    r.mcu.render(r.link);
    const Sent s = r.port.take();
    CHECK(contains(s, {0x90, McuProtocol::kNoteMute + 1, 0x7F}));
    CHECK(!contains(s, {0x90, McuProtocol::kNoteMute + 1, 0x00}));
    // Clamped at the last page, and channel shift moves by one.
    r.midi({0x90, McuProtocol::kNoteBankRight, 0x7F});
    r.midi({0x90, McuProtocol::kNoteBankRight, 0x7F});
    r.midi({0x90, McuProtocol::kNoteBankRight, 0x7F});
    CHECK(r.mcu.bank_offset() == kMaxStrips - kMcuStrips);
    r.midi({0x90, McuProtocol::kNoteChannelLeft, 0x7F});
    CHECK(r.mcu.bank_offset() == kMaxStrips - kMcuStrips - 1);
    r.midi({0x90, McuProtocol::kNoteBankLeft, 0x7F});
    r.midi({0x90, McuProtocol::kNoteBankLeft, 0x7F});
    r.midi({0x90, McuProtocol::kNoteBankLeft, 0x7F});
    CHECK(r.mcu.bank_offset() == 0);
}

void bank_change_redraws_from_cache() {
    Rig r;
    r.tell(FeedbackKind::Fader, 9, false, 0.5f);
    r.mcu.render(r.link);
    r.port.take();
    r.midi({0x90, McuProtocol::kNoteBankRight, 0x7F});
    r.mcu.render(r.link);
    // 0.5 -> 8192 -> LSB 0, MSB 0x40, on lane 1.
    CHECK(contains(r.port.take(), {0xE1, 0x00, 0x40}));
}

void fader_both_ways_and_touch() {
    Rig r;
    // Device to engine, with a touch bracket.
    r.midi({0x90, McuProtocol::kNoteTouch + 2, 0x7F});
    SurfaceEvent e{};
    CHECK(r.event(e) && e.kind == EventKind::Fader && e.strip == 2 && e.flag);
    r.midi({0xE2, 0x7F, 0x7F});
    CHECK(r.event(e) && e.strip == 2 && e.flag && e.value == 1.0f);
    // Engine to device while the finger is down: the motor stays still.
    r.tell(FeedbackKind::Fader, 2, false, 0.25f);
    r.mcu.render(r.link);
    CHECK(r.port.take().empty());
    r.midi({0x90, McuProtocol::kNoteTouch + 2, 0x00});
    CHECK(r.event(e) && e.kind == EventKind::Fader && !e.flag);
    // Released: the motor takes the engine's value.
    r.tell(FeedbackKind::Fader, 2, false, 0.25f);
    r.mcu.render(r.link);
    const int v = static_cast<int>(std::lround(0.25f * 16383.0f));
    CHECK(contains(r.port.take(), {0xE2, static_cast<std::uint8_t>(v & 0x7F),
                                   static_cast<std::uint8_t>(v >> 7)}));
    // Engine to device untouched: moves the motor; an unchanged value sends nothing.
    r.tell(FeedbackKind::Fader, 0, false, 1.0f);
    r.mcu.render(r.link);
    CHECK(contains(r.port.take(), {0xE0, 0x7F, 0x7F}));
    r.tell(FeedbackKind::Fader, 0, false, 1.0f);
    r.mcu.render(r.link);
    CHECK(r.port.take().empty());
    // No touch sense: a plain move is a Move.
    r.midi({0xE0, 0x00, 0x00});
    CHECK(r.event(e) && !e.flag && e.value == 0.0f);
}

void scribble_text() {
    Rig r;
    SurfaceFeedback f{};
    f.kind = FeedbackKind::Scribble;
    f.strip = 1;
    f.set_text("Vox");
    r.link.push_feedback(f);
    r.mcu.render(r.link);
    const Sent s = r.port.take();
    // F0 00 00 66 14 12 <offset 7> "Vox    " F7
    CHECK(contains(s, {0xF0, 0x00, 0x00, 0x66, 0x14, 0x12, 7, 'V', 'o', 'x', ' ', ' ', ' ',
                       ' ', 0xF7}));
    // Unchanged text is not sent again.
    r.link.push_feedback(f);
    r.mcu.render(r.link);
    CHECK(r.port.take().empty());
}

void transport_buttons_and_lights() {
    Rig r;
    SurfaceEvent e{};
    r.midi({0x90, McuProtocol::kNotePlay, 0x7F, 0x90, McuProtocol::kNotePlay, 0x00});
    CHECK(r.event(e) && e.kind == EventKind::Transport && e.transport == TransportCmd::Play);
    CHECK(!r.event(e));  // the release is not a second press
    r.midi({0x90, McuProtocol::kNoteStop, 0x7F});
    CHECK(r.event(e) && e.transport == TransportCmd::Stop);
    r.midi({0x90, McuProtocol::kNoteRecord, 0x7F});
    CHECK(r.event(e) && e.transport == TransportCmd::Record);
    r.midi({0x90, McuProtocol::kNoteRewind, 0x7F});
    CHECK(r.event(e) && e.transport == TransportCmd::Rewind && e.value == 0.0f);

    SurfaceFeedback f{};
    f.kind = FeedbackKind::Transport;
    f.transport = TransportCmd::Play;
    f.flag = true;
    r.link.push_feedback(f);
    r.mcu.render(r.link);
    CHECK(contains(r.port.take(), {0x90, McuProtocol::kNotePlay, 0x7F}));
}

void jog_and_vpot() {
    Rig r;
    SurfaceEvent e{};
    r.midi({0xB0, McuProtocol::kCcJog, 0x03});
    CHECK(r.event(e) && e.transport == TransportCmd::Forward && e.value == 3.0f);
    r.midi({0xB0, McuProtocol::kCcJog, 0x41});
    CHECK(r.event(e) && e.transport == TransportCmd::Rewind && e.value == 1.0f);

    r.tell(FeedbackKind::Pan, 0, false, -1.0f);
    r.mcu.render(r.link);
    CHECK(contains(r.port.take(), {0xB0, McuProtocol::kCcRing, 1}));  // hard left: first LED
    r.tell(FeedbackKind::Pan, 0, false, 0.0f);
    r.mcu.render(r.link);
    // Centre: position 6 with the centre LED (bit 6).
    CHECK(contains(r.port.take(), {0xB0, McuProtocol::kCcRing, 6 | 0x40}));
    r.midi({0xB0, McuProtocol::kCcVPot, 0x06});  // six detents right
    CHECK(r.event(e) && e.kind == EventKind::Pan && e.strip == 0);
    CHECK(std::fabs(e.value - 0.5f) < 1e-4f);
    r.midi({0xB0, McuProtocol::kCcVPot, 0x41});  // one back
    CHECK(r.event(e) && std::fabs(e.value - (0.5f - 1.0f / 12.0f)) < 1e-4f);
    r.midi({0x90, McuProtocol::kNoteVPush, 0x7F});
    CHECK(r.event(e) && e.kind == EventKind::Pan && e.value == 0.0f);
}

void buttons_toggle_from_known_state() {
    Rig r;
    SurfaceEvent e{};
    r.tell(FeedbackKind::Arm, 1, true);
    r.mcu.render(r.link);
    r.port.take();
    r.midi({0x90, McuProtocol::kNoteRec + 1, 0x7F});
    CHECK(r.event(e) && e.kind == EventKind::Arm && e.strip == 1 && !e.flag);
    r.midi({0x90, McuProtocol::kNoteSolo, 0x7F});
    CHECK(r.event(e) && e.kind == EventKind::Solo && e.strip == 0 && e.flag);
    r.midi({0x90, McuProtocol::kNoteSelect + 4, 0x7F});
    CHECK(r.event(e) && e.kind == EventKind::Select && e.strip == 4);
}

void meters_are_rate_limited() {
    Rig r;
    r.now = 2000;
    r.tell(FeedbackKind::Meter, 0, false, 0.5f);
    r.mcu.render(r.link);
    CHECK(contains(r.port.take(), {0xD0, 0x06}));
    // Ten updates inside the interval, one per millisecond.
    for (int i = 0; i < 10; ++i) {
        ++r.now;
        r.tell(FeedbackKind::Meter, 0, false, i % 2 ? 1.0f : 0.25f);
        r.mcu.render(r.link);
        CHECK(r.port.take().empty());
    }
    r.now = 2060;
    r.mcu.render(r.link);
    const Sent s = r.port.take();
    CHECK(s.size() == 1);
    CHECK(contains(s, {0xD0, 0x0C}));  // the latest wins: 1.0 -> 12
    // Strip 3's bar uses its lane in the high nibble.
    r.now = 2200;
    r.tell(FeedbackKind::Meter, 3, false, 1.0f);
    r.mcu.render(r.link);
    CHECK(contains(r.port.take(), {0xD0, 0x3C}));
}

void time_display() {
    Rig r;
    r.mcu.set_time_display("01.2");
    r.mcu.render(r.link);
    const Sent s = r.port.take();
    // Digit 0 is the rightmost on the device: CC 0x49 carries the leftmost text digit.
    CHECK(contains(s, {0xB0, 0x49, '0'}));
    CHECK(contains(s, {0xB0, 0x48, '1' | 0x40}));  // point after the 1
    CHECK(contains(s, {0xB0, 0x47, '2'}));
    r.mcu.render(r.link);
    CHECK(r.port.take().empty());
}

void parser_survives_chunks_and_running_status() {
    Rig r;
    SurfaceEvent e{};
    r.midi({0xE0, 0x00});
    CHECK(!r.event(e));
    r.midi({0x40});
    CHECK(r.event(e) && e.kind == EventKind::Fader);
    r.midi({0xE1, 0x00, 0x40, 0x00, 0x20});  // running status: a second bend
    CHECK(r.event(e) && e.strip == 1);
    CHECK(r.event(e) && e.strip == 1 && e.value < 0.3f);
    // A SysEx reply from the device (a connection query) is skipped whole.
    r.midi({0xF0, 0x00, 0x00, 0x66, 0x14, 0x01, 0x10, 0x20, 0xF7});
    r.midi({0xE0, 0x00, 0x00});
    CHECK(r.event(e) && e.strip == 0);
}

void a_rejected_write_is_retried_by_the_diff() {
    Rig r;
    r.port.reject_writes(true);
    r.tell(FeedbackKind::Fader, 0, false, 1.0f);
    r.mcu.render(r.link);
    r.port.reject_writes(false);
    // The protocol marks it sent either way; a later change is what resends.
    r.tell(FeedbackKind::Fader, 0, false, 0.5f);
    r.mcu.render(r.link);
    CHECK(contains(r.port.take(), {0xE0, 0x00, 0x40}));
}

}  // namespace

int main() {
    bank_to_tracks_9_to_16();
    bank_change_redraws_from_cache();
    fader_both_ways_and_touch();
    scribble_text();
    transport_buttons_and_lights();
    jog_and_vpot();
    buttons_toggle_from_known_state();
    meters_are_rate_limited();
    time_display();
    parser_survives_chunks_and_running_status();
    a_rejected_write_is_retried_by_the_diff();
    if (failures == 0) std::printf("mcu: ok\n");
    return failures == 0 ? 0 : 1;
}
