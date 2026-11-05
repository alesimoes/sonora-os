// SPDX-License-Identifier: GPL-3.0-or-later
// The HUI protocol against a scripted device. Every expected byte string is
// written out literally from brain/research/hui-protocol.md, not computed by
// the code under test.
#include <cmath>
#include <cstdio>
#include <vector>

#include "fake_midi_port.h"
#include "sonora/surfaces/hui.h"

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
    HuiProtocol hui;
    Rig() : hui(port, make_config(&now)) {
        hui.render(link);
        port.take();  // the first render pings and draws the whole page
    }
    static HuiProtocol::Config make_config(std::uint64_t* t) {
        HuiProtocol::Config c;
        c.clock_ms = [t] { return *t; };
        return c;
    }
    void midi(std::initializer_list<std::uint8_t> b) {
        const Bytes v(b);
        hui.on_midi(v.data(), v.size(), link);
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
    // What the surface sends when a switch at (zone, port) changes.
    void press(std::uint8_t zone, std::uint8_t port, bool on = true) {
        midi({0xB0, 0x0F, zone, 0xB0, 0x2F, static_cast<std::uint8_t>((on ? 0x40 : 0) | port)});
    }
};

void ping_keeps_the_surface_online() {
    FakeMidiPort port;
    SurfaceLink link;
    std::uint64_t now = 5000;
    HuiProtocol::Config c;
    c.clock_ms = [&now] { return now; };
    HuiProtocol hui(port, c);
    hui.render(link);
    CHECK(contains(port.take(), {0x90, 0x00, 0x00}));
    now += 999;
    hui.render(link);
    CHECK(!contains(port.take(), {0x90, 0x00, 0x00}));
    now += 1;
    hui.render(link);
    CHECK(contains(port.take(), {0x90, 0x00, 0x00}));

    CHECK(!hui.online());
    const Bytes reply = {0x90, 0x00, 0x7F};
    hui.on_midi(reply.data(), reply.size(), link);
    CHECK(hui.online());
    now += 3001;  // no reply for three intervals
    hui.render(link);
    CHECK(!hui.online());

    hui.on_midi(reply.data(), reply.size(), link);
    CHECK(hui.online());
    const Bytes reset = {0xFF};  // power cycle: everything it showed is gone
    hui.on_midi(reset.data(), reset.size(), link);
    CHECK(!hui.online());
    hui.render(link);
    CHECK(contains(port.take(), {0xB0, 0x10, 0x46}));  // the page is drawn again
}

void fader_both_ways_and_touch() {
    Rig r;
    SurfaceEvent e{};
    // Touch strip 2: zone 2, port 0 on.
    r.press(2, 0);
    CHECK(r.event(e) && e.kind == EventKind::Fader && e.strip == 2 && e.flag);
    // Move it to 8192: hi 0x40, lo 0x00, as the pair b0 02 40 / b0 22 00.
    r.midi({0xB0, 0x02, 0x40, 0xB0, 0x22, 0x00});
    CHECK(r.event(e) && e.kind == EventKind::Fader && e.strip == 2 && e.flag);
    CHECK(std::fabs(e.value - 8192.0f / 16383.0f) < 1e-4f);
    // The engine says something else while a finger is on it: the motor stays put.
    r.tell(FeedbackKind::Fader, 2, false, 1.0f);
    r.hui.render(r.link);
    CHECK(r.port.take().empty());
    // Release: the motor takes the engine's value (1.0 -> 16383 = 7f / 7f).
    r.press(2, 0, false);
    CHECK(r.event(e) && !e.flag);
    r.hui.render(r.link);
    CHECK(contains(r.port.take(), {0xB0, 0x02, 0x7F, 0xB0, 0x22, 0x7F}));
    // An untouched fader is driven: 0.5 -> 8192 on strip 5.
    r.tell(FeedbackKind::Fader, 5, false, 0.5f);
    r.hui.render(r.link);
    CHECK(contains(r.port.take(), {0xB0, 0x05, 0x40, 0xB0, 0x25, 0x00}));
    // A move with no touch sense is a plain move.
    r.midi({0xB0, 0x00, 0x7F, 0xB0, 0x20, 0x7F});
    CHECK(r.event(e) && e.strip == 0 && !e.flag && e.value > 0.999f);
}

void buttons_toggle_from_known_state() {
    Rig r;
    SurfaceEvent e{};
    r.press(3, HuiProtocol::kPortSolo);  // fifth-strip example in the report is zone 4
    CHECK(r.event(e) && e.kind == EventKind::Solo && e.strip == 3 && e.flag);
    r.press(3, HuiProtocol::kPortSolo, false);  // release does nothing
    CHECK(!r.event(e));
    r.tell(FeedbackKind::Solo, 3, true);
    r.hui.render(r.link);
    CHECK(contains(r.port.take(), {0xB0, 0x0C, 0x03, 0x2C, 0x43}));
    r.press(3, HuiProtocol::kPortSolo);
    CHECK(r.event(e) && e.kind == EventKind::Solo && !e.flag);  // from the known state

    r.press(1, HuiProtocol::kPortMute);
    CHECK(r.event(e) && e.kind == EventKind::Mute && e.strip == 1 && e.flag);
    r.press(6, HuiProtocol::kPortRecRdy);
    CHECK(r.event(e) && e.kind == EventKind::Arm && e.strip == 6 && e.flag);
    r.press(7, HuiProtocol::kPortSelect);
    CHECK(r.event(e) && e.kind == EventKind::Select && e.strip == 7);

    r.tell(FeedbackKind::Mute, 1, true);
    r.tell(FeedbackKind::Arm, 0, true);
    r.hui.render(r.link);
    const Sent s = r.port.take();
    CHECK(contains(s, {0xB0, 0x0C, 0x01, 0x2C, 0x42}));
    CHECK(contains(s, {0xB0, 0x0C, 0x00, 0x2C, 0x47}));
    r.hui.render(r.link);
    CHECK(r.port.take().empty());  // unchanged state is not resent
}

void a_port_switch_without_a_zone_is_ignored() {
    Rig r;
    SurfaceEvent e{};
    r.midi({0xB0, 0x2F, 0x42});
    CHECK(!r.event(e));
}

void bank_to_tracks_9_to_16() {
    Rig r;
    r.press(HuiProtocol::kZoneChannelSelect, HuiProtocol::kPortBankRight);
    CHECK(r.hui.bank_offset() == 8);
    SurfaceEvent e{};
    r.midi({0xB0, 0x00, 0x40, 0xB0, 0x20, 0x00});
    CHECK(r.event(e) && e.kind == EventKind::Fader && e.strip == 8);
    r.tell(FeedbackKind::Mute, 9, true);
    r.hui.render(r.link);
    CHECK(contains(r.port.take(), {0xB0, 0x0C, 0x01, 0x2C, 0x42}));
    for (int i = 0; i < 3; ++i) {
        r.press(HuiProtocol::kZoneChannelSelect, HuiProtocol::kPortBankRight);
    }
    CHECK(r.hui.bank_offset() == kMaxStrips - kHuiStrips);
    r.press(HuiProtocol::kZoneChannelSelect, HuiProtocol::kPortChannelLeft);
    CHECK(r.hui.bank_offset() == kMaxStrips - kHuiStrips - 1);
    for (int i = 0; i < 4; ++i) {
        r.press(HuiProtocol::kZoneChannelSelect, HuiProtocol::kPortBankLeft);
    }
    CHECK(r.hui.bank_offset() == 0);
}

void vpot_and_ring() {
    Rig r;
    SurfaceEvent e{};
    r.midi({0xB0, 0x40, 0x41});  // +1 on pot 0
    CHECK(r.event(e) && e.kind == EventKind::Pan && e.strip == 0);
    CHECK(std::fabs(e.value - 1.0f / 12.0f) < 1e-4f);
    r.midi({0xB0, 0x41, 0x03});  // -3 on pot 1
    CHECK(r.event(e) && e.strip == 1 && std::fabs(e.value + 0.25f) < 1e-4f);
    r.midi({0xB0, 0x40, 0x40});  // delta 0 never happens on a device; ignored
    CHECK(!r.event(e));
    r.press(1, HuiProtocol::kPortVSel);  // pot push centres
    CHECK(r.event(e) && e.kind == EventKind::Pan && e.strip == 1 && e.value == 0.0f);
    r.tell(FeedbackKind::Pan, 2, false, -1.0f);
    r.tell(FeedbackKind::Pan, 3, false, 1.0f);
    r.hui.render(r.link);
    const Sent s = r.port.take();
    CHECK(contains(s, {0xB0, 0x12, 0x01}));  // hard left: led 0, dot mode
    CHECK(contains(s, {0xB0, 0x13, 0x0B}));  // hard right: led 10
    r.tell(FeedbackKind::Pan, 3, false, 0.0f);
    r.hui.render(r.link);
    CHECK(contains(r.port.take(), {0xB0, 0x13, 0x46}));  // centre led 5 plus the small led
}

void scribble_text() {
    Rig r;
    SurfaceFeedback f{};
    f.kind = FeedbackKind::Scribble;
    f.strip = 2;
    f.set_text("Guitar L");
    r.link.push_feedback(f);
    r.hui.render(r.link);
    // The four-character display takes the first four.
    CHECK(contains(r.port.take(), {0xF0, 0x00, 0x00, 0x66, 0x05, 0x00, 0x10, 0x02, 'G', 'u', 'i',
                                   't', 0xF7}));
    r.hui.render(r.link);
    CHECK(r.port.take().empty());
}

void transport_buttons_jog_and_lights() {
    Rig r;
    SurfaceEvent e{};
    r.press(HuiProtocol::kZoneTransport, HuiProtocol::kPortPlay);
    CHECK(r.event(e) && e.kind == EventKind::Transport && e.transport == TransportCmd::Play);
    r.press(HuiProtocol::kZoneTransport, HuiProtocol::kPortStop);
    CHECK(r.event(e) && e.transport == TransportCmd::Stop);
    r.press(HuiProtocol::kZoneTransport, HuiProtocol::kPortRecord);
    CHECK(r.event(e) && e.transport == TransportCmd::Record);
    r.press(HuiProtocol::kZoneLoop, HuiProtocol::kPortLoop);
    CHECK(r.event(e) && e.transport == TransportCmd::Loop);
    r.press(HuiProtocol::kZoneTransport, HuiProtocol::kPortPlay, false);
    CHECK(!r.event(e));

    r.midi({0xB0, 0x0D, 0x43});  // jog +3
    CHECK(r.event(e) && e.transport == TransportCmd::Forward && e.value == 3.0f);
    r.midi({0xB0, 0x0D, 0x02});  // jog -2
    CHECK(r.event(e) && e.transport == TransportCmd::Rewind && e.value == 2.0f);

    SurfaceFeedback f{};
    f.kind = FeedbackKind::Transport;
    f.transport = TransportCmd::Play;
    f.flag = true;
    r.link.push_feedback(f);
    f.transport = TransportCmd::Loop;
    r.link.push_feedback(f);
    r.hui.render(r.link);
    const Sent s = r.port.take();
    CHECK(contains(s, {0xB0, 0x0C, 0x0E, 0x2C, 0x44}));
    CHECK(contains(s, {0xB0, 0x0C, 0x0F, 0x2C, 0x43}));
}

void meters_are_rate_limited() {
    Rig r;
    r.now = 2000;
    r.tell(FeedbackKind::Meter, 0, false, 0.5f);
    r.hui.render(r.link);
    CHECK(contains(r.port.take(), {0xA0, 0x00, 0x06, 0xA0, 0x00, 0x16}));
    for (int i = 0; i < 10; ++i) {
        ++r.now;
        r.tell(FeedbackKind::Meter, 0, false, i % 2 ? 1.0f : 0.25f);
        r.hui.render(r.link);
        CHECK(r.port.take().empty());
    }
    r.now = 2060;
    r.hui.render(r.link);
    const Sent s = r.port.take();
    CHECK(s.size() == 1 && contains(s, {0xA0, 0x00, 0x0C, 0xA0, 0x00, 0x1C}));
    r.now = 2200;
    r.tell(FeedbackKind::Meter, 3, false, 1.0f);
    r.hui.render(r.link);
    CHECK(contains(r.port.take(), {0xA0, 0x03, 0x0C, 0xA0, 0x03, 0x1C}));
}

void time_display() {
    Rig r;
    r.hui.set_time_display("01.2");
    r.hui.render(r.link);
    // Rightmost digit first; the point sits on the second digit from the left
    // (0x11 is '1.'), and the rightmost digit has no point.
    CHECK(contains(r.port.take(), {0xF0, 0x00, 0x00, 0x66, 0x05, 0x00, 0x11, 0, 0, 0, 0, 0, 0x02,
                                   0x11, 0x00, 0xF7}));
    r.hui.render(r.link);
    CHECK(r.port.take().empty());
    r.hui.set_time_display("0000000F");
    r.hui.render(r.link);
    CHECK(contains(r.port.take(), {0xF0, 0x00, 0x00, 0x66, 0x05, 0x00, 0x11, 0x0F, 0, 0, 0, 0, 0,
                                   0, 0, 0xF7}));
}

void parser_survives_chunks_and_foreign_traffic() {
    Rig r;
    SurfaceEvent e{};
    r.midi({0xB0, 0x0F});
    r.midi({0x01, 0xB0, 0x2F});
    CHECK(!r.event(e));
    r.midi({0x42});  // zone 1 port 2 on: mute
    CHECK(r.event(e) && e.kind == EventKind::Mute && e.strip == 1);
    // A SysEx frame in the middle, and a realtime byte inside a message.
    r.midi({0xF0, 0x00, 0x00, 0x66, 0x05, 0x00, 0x11, 0x01, 0xF7});
    r.midi({0xB0, 0x40, 0xF8, 0x41});
    CHECK(r.event(e) && e.kind == EventKind::Pan);
    // Note-ons that are not the ping reply are ignored.
    r.midi({0x90, 0x01, 0x7F});
    CHECK(!r.event(e) && !r.hui.online());
}

void a_rejected_write_is_retried_by_the_diff() {
    Rig r;
    r.port.reject_writes(true);
    r.tell(FeedbackKind::Fader, 0, false, 1.0f);
    r.hui.render(r.link);
    r.port.reject_writes(false);
    r.tell(FeedbackKind::Fader, 0, false, 0.5f);
    r.hui.render(r.link);
    CHECK(contains(r.port.take(), {0xB0, 0x00, 0x40, 0xB0, 0x20, 0x00}));
}

}  // namespace

int main() {
    ping_keeps_the_surface_online();
    fader_both_ways_and_touch();
    buttons_toggle_from_known_state();
    a_port_switch_without_a_zone_is_ignored();
    bank_to_tracks_9_to_16();
    vpot_and_ring();
    scribble_text();
    transport_buttons_jog_and_lights();
    meters_are_rate_limited();
    time_display();
    parser_survives_chunks_and_foreign_traffic();
    a_rejected_write_is_retried_by_the_diff();
    if (failures == 0) std::printf("hui: ok\n");
    return failures == 0 ? 0 : 1;
}
