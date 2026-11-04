// SPDX-License-Identifier: GPL-3.0-or-later
// The Mackie Control Universal protocol on the surface thread (ADR-0055),
// written from published descriptions only (brain/research/mcu-protocol.md);
// no vendor code. It decodes the device's MIDI into SurfaceEvents and
// diffs the engine's SurfaceFeedback into MIDI. It owns no engine state.
//
// Banking lives here, not in the engine side (ADR-0055 amends ADR-0040): the
// MCU has eight physical strips and bank / channel buttons, and the engine
// must not learn what a button press means. Events carry the absolute strip
// (bank offset + physical strip), feedback is cached for all kMaxStrips and
// the eight in view are rendered, so a bank change redraws from the cache
// with no round trip.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "sonora/surfaces/midi_port.h"
#include "sonora/surfaces/surface_link.h"

namespace sonora::surfaces {

constexpr std::size_t kMcuStrips = 8;

class McuProtocol : public SurfaceProtocol {
public:
    struct Config {
        // Monotonic milliseconds. Empty: no rate limit on meters.
        std::function<std::uint64_t()> clock_ms;
        // Meter refresh. The device decays a bar by itself, so 20 Hz is
        // plenty and keeps a MIDI cable (3125 B/s) far from full.
        std::uint64_t meter_interval_ms = 50;
        // One V-Pot detent moves pan by this much.
        float pan_step = 1.0f / 12.0f;
    };

    McuProtocol(MidiPort& port, Config cfg);

    void on_midi(const std::uint8_t* bytes, std::size_t n, SurfaceLink& link) override;
    void render(SurfaceLink& link) override;

    // The surface thread's tick: read the port, decode, render.
    void poll(SurfaceLink& link);

    // The 10-digit time display, leftmost digit first, e.g. "00.01.02.03 "
    // style text is the caller's to format; a '.' lights the previous digit's
    // decimal point and takes no digit. The contract has no time feedback
    // yet (ADR-0055), so the owner of the surface thread sets it.
    void set_time_display(const char* text);

    std::size_t bank_offset() const { return offset_; }

    // The connection handshake (brain/research/mcu-protocol.md, sources 1 and
    // 2). query_device() writes the Device Query; the unit answers with a
    // Host Connection Query (serial + challenge), on_midi() replies with the
    // serial and the response code, and the unit confirms. A device that
    // never asks (Logic Control compatible ones) is simply never "online":
    // rendering does not wait for it, so those keep working. The confirmation
    // redraws the page, since a unit that connects clears its displays.
    void query_device();
    bool online() const { return online_; }
    bool connection_refused() const { return refused_; }
    // Public so the test can pin the published vectors.
    static void challenge_response(const std::uint8_t challenge[4], std::uint8_t response[4]);

    // The note numbers, public so tests and docs name them once. Every value
    // is in the table of brain/research/mcu-protocol.md with its source.
    static constexpr std::uint8_t kNoteRec = 0x00, kNoteSolo = 0x08, kNoteMute = 0x10,
                                  kNoteSelect = 0x18, kNoteVPush = 0x20, kNoteBankLeft = 0x2E,
                                  kNoteBankRight = 0x2F, kNoteChannelLeft = 0x30,
                                  kNoteChannelRight = 0x31, kNoteCycle = 0x56,
                                  kNoteRewind = 0x5B, kNoteForward = 0x5C, kNoteStop = 0x5D,
                                  kNotePlay = 0x5E, kNoteRecord = 0x5F, kNoteTouch = 0x68;
    static constexpr std::uint8_t kCcVPot = 0x10, kCcRing = 0x30, kCcJog = 0x3C, kCcTime = 0x40;

private:
    void sysex(SurfaceLink& link);
    void message(std::uint8_t status, std::uint8_t d1, std::uint8_t d2, SurfaceLink& link);
    void note(std::uint8_t n, bool on, SurfaceLink& link);
    void invalidate_page();
    void out(const std::uint8_t* b, std::size_t n);
    void send_event(SurfaceLink& link, EventKind k, std::uint8_t strip, bool flag, float value);
    void send_transport(SurfaceLink& link, TransportCmd c, float value);

    MidiPort& port_;
    Config cfg_;

    // Stream parser: chunks may split a message; running status is honoured.
    std::uint8_t status_ = 0;
    std::uint8_t data_[2] = {0, 0};
    std::uint8_t have_ = 0;
    bool in_sysex_ = false;
    // Bytes between F0 and F7; the longest message we act on is 16.
    static constexpr std::size_t kSysexCap = 32;
    std::uint8_t sysex_[kSysexCap];
    std::size_t sysex_len_ = 0;
    bool sysex_over_ = false;
    bool online_ = false;
    bool refused_ = false;

    std::size_t offset_ = 0;  // absolute strip shown on physical strip 0

    // What the engine last said, per absolute strip.
    float fader_[kMaxStrips] = {};
    float pan_[kMaxStrips] = {};
    bool mute_[kMaxStrips] = {};
    bool solo_[kMaxStrips] = {};
    bool arm_[kMaxStrips] = {};
    bool touched_[kMaxStrips] = {};
    std::uint8_t level_[kMaxStrips] = {};
    char text_[kMaxStrips][kScribbleChars];
    bool lit_[7] = {};

    // What the device was last told, per physical strip; -1 means unknown.
    int sent_fader_[kMcuStrips];
    int sent_ring_[kMcuStrips];
    int sent_button_[3][kMcuStrips];
    int sent_level_[kMcuStrips];
    char sent_text_[kMcuStrips][kScribbleChars];
    bool sent_text_valid_[kMcuStrips];
    int sent_lit_[7];
    char time_want_[10];
    int time_sent_[10];
    std::uint64_t last_meter_ms_ = 0;
    bool meter_started_ = false;
};

}  // namespace sonora::surfaces
