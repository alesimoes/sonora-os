// SPDX-License-Identifier: GPL-3.0-or-later
// The HUI protocol on the surface thread (ADR-0055), written from the
// published reverse-engineering report cited in brain/research/hui-protocol.md;
// no vendor code. Same shape as McuProtocol: decode the device's MIDI into
// SurfaceEvents, diff the engine's SurfaceFeedback into MIDI, own no engine
// state, and bank here so the engine never learns what a bank button is.
//
// HUI addresses every switch and LED as a (zone, port) pair: a zone is
// selected with one control change, then a port is switched with another.
// Faders and V-Pots have their own controllers. A surface that is not pinged
// about once a second goes offline and stops obeying its faders.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "sonora/surfaces/midi_port.h"
#include "sonora/surfaces/surface_link.h"

namespace sonora::surfaces {

constexpr std::size_t kHuiStrips = 8;
constexpr std::size_t kHuiScribbleChars = 4;

class HuiProtocol : public SurfaceProtocol {
public:
    struct Config {
        // Monotonic milliseconds. Empty: no rate limit on meters, and the
        // only ping is the one in the first render().
        std::function<std::uint64_t()> clock_ms;
        std::uint64_t meter_interval_ms = 50;
        // "About every second" (report, Ping).
        std::uint64_t ping_interval_ms = 1000;
        float pan_step = 1.0f / 12.0f;
    };

    HuiProtocol(MidiPort& port, Config cfg);

    void on_midi(const std::uint8_t* bytes, std::size_t n, SurfaceLink& link) override;
    void render(SurfaceLink& link) override;
    void poll(SurfaceLink& link);

    // The 8-digit time display, leftmost digit first, '.' lights the point of
    // the digit before it. Only 0-9 and A-F are documented; anything else
    // shows '0'. The contract has no time feedback (ADR-0055).
    void set_time_display(const char* text);

    // True once the surface answered a ping and has not been silent for three
    // intervals. Faders are driven either way: a missing reply is reported,
    // not obeyed.
    bool online() const { return online_; }
    std::size_t bank_offset() const { return offset_; }

    // Zones and ports (report, "Zone-names" and hardware layout). Strips are
    // zones 0-7. The transport and channel-select port order is read from the
    // layout table, not stated in prose: check it on a device.
    static constexpr std::uint8_t kPortTouch = 0, kPortSelect = 1, kPortMute = 2, kPortSolo = 3,
                                  kPortVSel = 5, kPortRecRdy = 7;
    static constexpr std::uint8_t kZoneChannelSelect = 0x0A;
    static constexpr std::uint8_t kPortChannelLeft = 0, kPortBankLeft = 1, kPortChannelRight = 2,
                                  kPortBankRight = 3;
    static constexpr std::uint8_t kZoneTransport = 0x0E;
    static constexpr std::uint8_t kPortRewind = 1, kPortForward = 2, kPortStop = 3, kPortPlay = 4,
                                  kPortRecord = 5;
    static constexpr std::uint8_t kZoneLoop = 0x0F;
    static constexpr std::uint8_t kPortLoop = 3;

    static constexpr std::uint8_t kCcFaderMsb = 0x00, kCcZoneSelectOut = 0x0C, kCcJog = 0x0D,
                                  kCcZoneSelectIn = 0x0F, kCcRing = 0x10, kCcFaderLsb = 0x20,
                                  kCcPortOut = 0x2C, kCcPortIn = 0x2F, kCcVPot = 0x40;

private:
    void message(std::uint8_t status, std::uint8_t d1, std::uint8_t d2, SurfaceLink& link);
    void port_switch(std::uint8_t zone, std::uint8_t port, bool on, SurfaceLink& link);
    void invalidate_page();
    void out(const std::uint8_t* b, std::size_t n);
    void send_event(SurfaceLink& link, EventKind k, std::uint8_t strip, bool flag, float value);
    void send_transport(SurfaceLink& link, TransportCmd c, float value);
    void led(std::uint8_t zone, std::uint8_t port, bool on);

    MidiPort& port_;
    Config cfg_;

    std::uint8_t status_ = 0;
    std::uint8_t data_[2] = {0, 0};
    std::uint8_t have_ = 0;
    bool in_sysex_ = false;

    std::uint8_t rx_zone_ = 0xFF;  // the zone the surface last announced
    std::uint8_t fader_msb_[kHuiStrips] = {};

    std::size_t offset_ = 0;
    bool online_ = false;
    bool pinged_ = false;
    std::uint64_t last_ping_ms_ = 0;
    std::uint64_t last_reply_ms_ = 0;

    float fader_[kMaxStrips] = {};
    float pan_[kMaxStrips] = {};
    bool mute_[kMaxStrips] = {};
    bool solo_[kMaxStrips] = {};
    bool arm_[kMaxStrips] = {};
    bool touched_[kMaxStrips] = {};
    std::uint8_t level_[kMaxStrips] = {};
    char text_[kMaxStrips][kScribbleChars];
    bool lit_[7] = {};

    int sent_fader_[kHuiStrips];
    int sent_ring_[kHuiStrips];
    int sent_button_[3][kHuiStrips];
    int sent_level_[kHuiStrips];
    char sent_text_[kHuiStrips][kHuiScribbleChars];
    bool sent_text_valid_[kHuiStrips];
    int sent_lit_[7];
    char time_want_[8];
    int time_sent_[8];
    std::uint64_t last_meter_ms_ = 0;
    bool meter_started_ = false;
};

}  // namespace sonora::surfaces
