// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/surfaces/hui.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace sonora::surfaces {

namespace {

constexpr std::uint8_t kSysexHeader[] = {0xF0, 0x00, 0x00, 0x66, 0x05, 0x00};
constexpr std::uint8_t kCmdSmallDisplay = 0x10;
constexpr std::uint8_t kCmdTimeDisplay = 0x11;
constexpr int kFaderMax = 16383;
constexpr std::uint8_t kPingOut[] = {0x90, 0x00, 0x00};
constexpr std::uint8_t kPingReplyVelocity = 0x7F;

int fader_to_14(float v) {
    return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * static_cast<float>(kFaderMax)));
}

// Dot mode runs 1..11 across the ring, 6 is the centre (report, V-Pot rings).
int pan_position(float pan) {
    const float p = std::clamp(pan, -1.0f, 1.0f);
    return 1 + static_cast<int>(std::lround((p + 1.0f) * 5.0f));
}

// Delta encoding shared by V-Pots and the jog wheel: above 0x40 is positive.
int delta_of(std::uint8_t v) {
    if (v > 0x40) return v - 0x40;
    if (v < 0x40) return -static_cast<int>(v);
    return 0;
}

int digit_code(char c, bool point) {
    int v = 0;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'A' && c <= 'F') v = 0x0A + (c - 'A');
    else if (c >= 'a' && c <= 'f') v = 0x0A + (c - 'a');
    return v | (point ? 0x10 : 0);
}

struct LightAddr {
    std::uint8_t zone, port;
};
LightAddr light_for(std::size_t t) {
    switch (static_cast<TransportCmd>(t)) {
        case TransportCmd::Play: return {HuiProtocol::kZoneTransport, HuiProtocol::kPortPlay};
        case TransportCmd::Stop: return {HuiProtocol::kZoneTransport, HuiProtocol::kPortStop};
        case TransportCmd::Record: return {HuiProtocol::kZoneTransport, HuiProtocol::kPortRecord};
        case TransportCmd::Rewind: return {HuiProtocol::kZoneTransport, HuiProtocol::kPortRewind};
        case TransportCmd::Forward: return {HuiProtocol::kZoneTransport, HuiProtocol::kPortForward};
        default: return {HuiProtocol::kZoneLoop, HuiProtocol::kPortLoop};
    }
}

}  // namespace

HuiProtocol::HuiProtocol(MidiPort& port, Config cfg) : port_(port), cfg_(std::move(cfg)) {
    for (auto& t : text_) std::memset(t, ' ', kScribbleChars);
    std::memset(time_want_, ' ', sizeof time_want_);
    invalidate_page();
}

void HuiProtocol::invalidate_page() {
    for (std::size_t i = 0; i < kHuiStrips; ++i) {
        sent_fader_[i] = -1;
        sent_ring_[i] = -1;
        for (auto& b : sent_button_) b[i] = -1;
        sent_level_[i] = -1;
        sent_text_valid_[i] = false;
    }
    for (int& v : sent_lit_) v = -1;
    for (int& v : time_sent_) v = -1;
}

void HuiProtocol::out(const std::uint8_t* b, std::size_t n) { (void)port_.write(b, n); }

void HuiProtocol::send_event(SurfaceLink& link, EventKind k, std::uint8_t strip, bool flag,
                             float value) {
    SurfaceEvent e{};
    e.kind = k;
    e.strip = strip;
    e.flag = flag;
    e.transport = TransportCmd::None;
    e.value = value;
    link.push_event(e);
}

void HuiProtocol::send_transport(SurfaceLink& link, TransportCmd c, float value) {
    SurfaceEvent e{};
    e.kind = EventKind::Transport;
    e.transport = c;
    e.flag = true;
    e.value = value;
    link.push_event(e);
}

void HuiProtocol::on_midi(const std::uint8_t* bytes, std::size_t n, SurfaceLink& link) {
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t b = bytes[i];
        if (b == 0xFF) {
            // The surface sends system reset when it is switched on or off
            // (report, System reset): everything it showed is gone.
            online_ = false;
            invalidate_page();
            rx_zone_ = 0xFF;
            status_ = 0;
            have_ = 0;
            continue;
        }
        if (b >= 0xF8) continue;
        if (b == 0xF0) {
            in_sysex_ = true;
            status_ = 0;
            have_ = 0;
            continue;
        }
        if (b == 0xF7) {  // the report documents no SysEx from the surface
            in_sysex_ = false;
            continue;
        }
        if (b & 0x80) {
            in_sysex_ = false;
            status_ = b;
            have_ = 0;
            continue;
        }
        if (in_sysex_ || status_ == 0) continue;
        data_[have_++] = b;
        if (have_ == 2) {
            message(status_, data_[0], data_[1], link);
            have_ = 0;
        }
    }
}

void HuiProtocol::message(std::uint8_t status, std::uint8_t d1, std::uint8_t d2,
                          SurfaceLink& link) {
    const std::uint8_t kind = status & 0xF0;
    if (kind == 0x90) {
        if (d1 == 0 && d2 == kPingReplyVelocity) {
            online_ = true;
            if (cfg_.clock_ms) last_reply_ms_ = cfg_.clock_ms();
        }
        return;
    }
    if (kind != 0xB0) return;

    if (d1 == kCcZoneSelectIn) {
        rx_zone_ = d2;
    } else if (d1 == kCcPortIn) {
        if (rx_zone_ != 0xFF) port_switch(rx_zone_, d2 & 0x07, (d2 & 0x40) != 0, link);
    } else if (d1 < kHuiStrips) {  // fader high bits; the low bits close the pair
        fader_msb_[d1] = d2;
    } else if (d1 >= kCcFaderLsb && d1 < kCcFaderLsb + kHuiStrips) {
        const std::size_t z = d1 - kCcFaderLsb;
        const std::size_t strip = offset_ + z;
        const int v = (fader_msb_[z] << 7) | d2;
        fader_[strip] = static_cast<float>(v) / static_cast<float>(kFaderMax);
        send_event(link, EventKind::Fader, static_cast<std::uint8_t>(strip), touched_[strip],
                   fader_[strip]);
    } else if (d1 >= kCcVPot && d1 < kCcVPot + kHuiStrips) {
        const int delta = delta_of(d2);
        if (delta == 0) return;
        const std::size_t strip = offset_ + (d1 - kCcVPot);
        pan_[strip] = std::clamp(pan_[strip] + cfg_.pan_step * static_cast<float>(delta), -1.0f,
                                 1.0f);
        send_event(link, EventKind::Pan, static_cast<std::uint8_t>(strip), false, pan_[strip]);
    } else if (d1 == kCcJog) {
        const int delta = delta_of(d2);
        if (delta != 0) {
            send_transport(link, delta < 0 ? TransportCmd::Rewind : TransportCmd::Forward,
                           static_cast<float>(std::abs(delta)));
        }
    }
}

void HuiProtocol::port_switch(std::uint8_t zone, std::uint8_t port, bool on,
                              SurfaceLink& link) {
    if (zone < kHuiStrips) {
        const std::size_t s = offset_ + zone;
        if (port == kPortTouch) {
            touched_[s] = on;
            if (!on) sent_fader_[zone] = -1;  // the motor returns to the engine's value
            send_event(link, EventKind::Fader, static_cast<std::uint8_t>(s), on, fader_[s]);
            return;
        }
        if (!on) return;  // buttons act on press
        if (port == kPortMute) {
            send_event(link, EventKind::Mute, static_cast<std::uint8_t>(s), !mute_[s], 0);
        } else if (port == kPortSolo) {
            send_event(link, EventKind::Solo, static_cast<std::uint8_t>(s), !solo_[s], 0);
        } else if (port == kPortRecRdy) {
            send_event(link, EventKind::Arm, static_cast<std::uint8_t>(s), !arm_[s], 0);
        } else if (port == kPortSelect) {
            send_event(link, EventKind::Select, static_cast<std::uint8_t>(s), true, 0);
        } else if (port == kPortVSel) {
            pan_[s] = 0;  // pushing the pot centres the pan, as on the MCU
            send_event(link, EventKind::Pan, static_cast<std::uint8_t>(s), false, 0);
        }
        return;
    }
    if (!on) return;
    if (zone == kZoneChannelSelect) {
        const std::size_t top = kMaxStrips - kHuiStrips;
        std::size_t next = offset_;
        if (port == kPortBankLeft) next = offset_ >= kHuiStrips ? offset_ - kHuiStrips : 0;
        if (port == kPortBankRight) next = std::min(top, offset_ + kHuiStrips);
        if (port == kPortChannelLeft) next = offset_ > 0 ? offset_ - 1 : 0;
        if (port == kPortChannelRight) next = std::min(top, offset_ + 1);
        if (next != offset_) {
            offset_ = next;
            invalidate_page();
        }
    } else if (zone == kZoneTransport) {
        if (port == kPortRewind) send_transport(link, TransportCmd::Rewind, 0);
        if (port == kPortForward) send_transport(link, TransportCmd::Forward, 0);
        if (port == kPortStop) send_transport(link, TransportCmd::Stop, 0);
        if (port == kPortPlay) send_transport(link, TransportCmd::Play, 0);
        if (port == kPortRecord) send_transport(link, TransportCmd::Record, 0);
    } else if (zone == kZoneLoop && port == kPortLoop) {
        send_transport(link, TransportCmd::Loop, 0);
    }
}

void HuiProtocol::set_time_display(const char* text) {
    std::size_t at = 0;
    for (std::size_t i = 0; text[i] != '\0' && at < sizeof time_want_; ++i) {
        if (text[i] == '.' && at > 0) {
            time_want_[at - 1] = static_cast<char>(time_want_[at - 1] | 0x80);
        } else {
            time_want_[at++] = text[i];
        }
    }
    for (; at < sizeof time_want_; ++at) time_want_[at] = ' ';
}

void HuiProtocol::led(std::uint8_t zone, std::uint8_t port, bool on) {
    // Zone select then port switch in one write, as the report shows
    // ("b0 0c 08 2c 40").
    const std::uint8_t m[] = {0xB0, kCcZoneSelectOut, zone, kCcPortOut,
                              static_cast<std::uint8_t>((on ? 0x40 : 0x00) | (port & 0x07))};
    out(m, sizeof m);
}

void HuiProtocol::render(SurfaceLink& link) {
    SurfaceFeedback f{};
    while (link.pop_feedback(f)) {
        if (f.kind == FeedbackKind::Transport) {
            const auto idx = static_cast<std::size_t>(f.transport);
            if (idx < 7) lit_[idx] = f.flag;
            continue;
        }
        if (f.strip >= kMaxStrips) continue;
        switch (f.kind) {
            case FeedbackKind::Fader: fader_[f.strip] = f.value; break;
            case FeedbackKind::Pan: pan_[f.strip] = f.value; break;
            case FeedbackKind::Mute: mute_[f.strip] = f.flag; break;
            case FeedbackKind::Solo: solo_[f.strip] = f.flag; break;
            case FeedbackKind::Arm: arm_[f.strip] = f.flag; break;
            case FeedbackKind::Meter:
                level_[f.strip] = static_cast<std::uint8_t>(
                    std::lround(std::clamp(f.value, 0.0f, 1.0f) * 12.0f));
                break;
            case FeedbackKind::Scribble: std::memcpy(text_[f.strip], f.text, kScribbleChars); break;
            default: break;
        }
    }

    bool meters_due = true;
    bool ping_due = !pinged_;
    if (cfg_.clock_ms) {
        const std::uint64_t now = cfg_.clock_ms();
        meters_due = !meter_started_ || now - last_meter_ms_ >= cfg_.meter_interval_ms;
        if (meters_due) {
            last_meter_ms_ = now;
            meter_started_ = true;
        }
        ping_due = !pinged_ || now - last_ping_ms_ >= cfg_.ping_interval_ms;
        if (ping_due) last_ping_ms_ = now;
        if (online_ && now - last_reply_ms_ > 3 * cfg_.ping_interval_ms) online_ = false;
    }
    if (ping_due) {
        out(kPingOut, sizeof kPingOut);
        pinged_ = true;
    }

    for (std::size_t i = 0; i < kHuiStrips; ++i) {
        const std::size_t s = offset_ + i;
        const auto lane = static_cast<std::uint8_t>(i);

        const int want_fader = fader_to_14(fader_[s]);
        if (!touched_[s] && want_fader != sent_fader_[i]) {
            const std::uint8_t m[] = {0xB0, static_cast<std::uint8_t>(kCcFaderMsb + lane),
                                      static_cast<std::uint8_t>(want_fader >> 7),
                                      0xB0, static_cast<std::uint8_t>(kCcFaderLsb + lane),
                                      static_cast<std::uint8_t>(want_fader & 0x7F)};
            out(m, sizeof m);
            sent_fader_[i] = want_fader;
        }

        const int pos = pan_position(pan_[s]);
        const int ring = pos | (pos == 6 ? 0x40 : 0);
        if (ring != sent_ring_[i]) {
            const std::uint8_t m[] = {0xB0, static_cast<std::uint8_t>(kCcRing + lane),
                                      static_cast<std::uint8_t>(ring)};
            out(m, sizeof m);
            sent_ring_[i] = ring;
        }

        const bool btn[3] = {arm_[s], solo_[s], mute_[s]};
        const std::uint8_t port[3] = {kPortRecRdy, kPortSolo, kPortMute};
        for (int b = 0; b < 3; ++b) {
            if (sent_button_[b][i] != (btn[b] ? 1 : 0)) {
                led(lane, port[b], btn[b]);
                sent_button_[b][i] = btn[b] ? 1 : 0;
            }
        }

        if (meters_due && level_[s] != sent_level_[i]) {
            // Left then right; a mono track shows on both.
            const std::uint8_t m[] = {0xA0, lane, level_[s],
                                      0xA0, lane, static_cast<std::uint8_t>(0x10 | level_[s])};
            out(m, sizeof m);
            sent_level_[i] = level_[s];
        }

        if (!sent_text_valid_[i] ||
            std::memcmp(sent_text_[i], text_[s], kHuiScribbleChars) != 0) {
            std::uint8_t m[sizeof kSysexHeader + 2 + kHuiScribbleChars + 1];
            std::size_t n = 0;
            for (const auto b : kSysexHeader) m[n++] = b;
            m[n++] = kCmdSmallDisplay;
            m[n++] = lane;
            for (std::size_t c = 0; c < kHuiScribbleChars; ++c) {
                const auto ch = static_cast<unsigned char>(text_[s][c]);
                m[n++] = (ch >= 0x20 && ch < 0x7F) ? ch : '?';
            }
            m[n++] = 0xF7;
            out(m, n);
            std::memcpy(sent_text_[i], text_[s], kHuiScribbleChars);
            sent_text_valid_[i] = true;
        }
    }

    for (std::size_t t = 1; t < 7; ++t) {
        if (sent_lit_[t] != (lit_[t] ? 1 : 0)) {
            const LightAddr a = light_for(t);
            led(a.zone, a.port, lit_[t]);
            sent_lit_[t] = lit_[t] ? 1 : 0;
        }
    }

    // Digits go on the wire least significant (rightmost) first, so text
    // index 7 is y0 and index 0 is y7. The whole frame is rewritten when any
    // digit changed: the report allows up to eight digits in one frame.
    int want[8];
    bool changed = false;
    for (std::size_t i = 0; i < sizeof time_want_; ++i) {
        const std::size_t y = sizeof time_want_ - 1 - i;
        want[y] = digit_code(static_cast<char>(time_want_[i] & 0x7F),
                             y != 0 && (time_want_[i] & 0x80) != 0);
        if (want[y] != time_sent_[y]) changed = true;
    }
    if (changed) {
        std::uint8_t m[sizeof kSysexHeader + 1 + 8 + 1];
        std::size_t n = 0;
        for (const auto b : kSysexHeader) m[n++] = b;
        m[n++] = kCmdTimeDisplay;
        for (std::size_t y = 0; y < 8; ++y) {
            m[n++] = static_cast<std::uint8_t>(want[y]);
            time_sent_[y] = want[y];
        }
        m[n++] = 0xF7;
        out(m, n);
    }
}

void HuiProtocol::poll(SurfaceLink& link) {
    std::uint8_t buf[256];
    for (;;) {
        const std::size_t n = port_.read(buf, sizeof buf);
        if (n == 0) break;
        on_midi(buf, n, link);
    }
    render(link);
}

}  // namespace sonora::surfaces
