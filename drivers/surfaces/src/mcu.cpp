// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/surfaces/mcu.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace sonora::surfaces {

namespace {

constexpr std::uint8_t kSysexHeader[] = {0xF0, 0x00, 0x00, 0x66, 0x14};
constexpr std::uint8_t kCmdLcd = 0x12;
constexpr std::uint8_t kCmdDeviceQuery = 0x00, kCmdConnectionQuery = 0x01,
                       kCmdConnectionReply = 0x02, kCmdConnectionConfirm = 0x03,
                       kCmdConnectionError = 0x04;
constexpr std::size_t kSerialLen = 7, kChallengeLen = 4;
constexpr int kFaderMax = 16383;

int fader_to_14(float v) {
    return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * static_cast<float>(kFaderMax)));
}

// Position 1..11 for -1..1; 0 is "all off" and is not used for a live pan.
int pan_position(float pan) {
    const float p = std::clamp(pan, -1.0f, 1.0f);
    return 1 + static_cast<int>(std::lround((p + 1.0f) * 5.0f));
}

std::uint8_t note_for(TransportCmd c) {
    switch (c) {
        case TransportCmd::Play: return McuProtocol::kNotePlay;
        case TransportCmd::Stop: return McuProtocol::kNoteStop;
        case TransportCmd::Record: return McuProtocol::kNoteRecord;
        case TransportCmd::Rewind: return McuProtocol::kNoteRewind;
        case TransportCmd::Forward: return McuProtocol::kNoteForward;
        case TransportCmd::Loop: return McuProtocol::kNoteCycle;
        default: return 0xFF;
    }
}

std::uint8_t time_code(char c, bool point) {
    return static_cast<std::uint8_t>((static_cast<unsigned char>(c) & 0x3F) | (point ? 0x40 : 0));
}

}  // namespace

McuProtocol::McuProtocol(MidiPort& port, Config cfg) : port_(port), cfg_(std::move(cfg)) {
    for (auto& t : text_) std::memset(t, ' ', kScribbleChars);
    std::memset(time_want_, ' ', sizeof time_want_);
    invalidate_page();
    for (int& v : time_sent_) v = -1;
}

void McuProtocol::invalidate_page() {
    for (std::size_t i = 0; i < kMcuStrips; ++i) {
        sent_fader_[i] = -1;
        sent_ring_[i] = -1;
        for (auto& b : sent_button_) b[i] = -1;
        sent_level_[i] = -1;
        sent_text_valid_[i] = false;
    }
    for (int& v : sent_lit_) v = -1;
}

void McuProtocol::out(const std::uint8_t* b, std::size_t n) { (void)port_.write(b, n); }

void McuProtocol::send_event(SurfaceLink& link, EventKind k, std::uint8_t strip, bool flag,
                             float value) {
    SurfaceEvent e{};
    e.kind = k;
    e.strip = strip;
    e.flag = flag;
    e.transport = TransportCmd::None;
    e.value = value;
    link.push_event(e);
}

void McuProtocol::send_transport(SurfaceLink& link, TransportCmd c, float value) {
    SurfaceEvent e{};
    e.kind = EventKind::Transport;
    e.transport = c;
    e.flag = true;
    e.value = value;
    link.push_event(e);
}

void McuProtocol::on_midi(const std::uint8_t* bytes, std::size_t n, SurfaceLink& link) {
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t b = bytes[i];
        if (b >= 0xF8) continue;  // realtime bytes may sit inside anything
        if (b == 0xF0) {
            in_sysex_ = true;
            sysex_len_ = 0;
            sysex_over_ = false;
            status_ = 0;
            have_ = 0;
            continue;
        }
        if (b == 0xF7) {
            if (in_sysex_ && !sysex_over_) sysex(link);
            in_sysex_ = false;
            continue;
        }
        if (b & 0x80) {
            in_sysex_ = false;
            status_ = b;
            have_ = 0;
            continue;
        }
        if (in_sysex_) {
            if (sysex_len_ < kSysexCap) {
                sysex_[sysex_len_++] = b;
            } else {
                sysex_over_ = true;  // longer than anything we act on
            }
            continue;
        }
        if (status_ == 0) continue;
        data_[have_++] = b;
        const std::uint8_t kind = status_ & 0xF0;
        const std::uint8_t need = (kind == 0xC0 || kind == 0xD0) ? 1 : 2;
        if (have_ == need) {
            message(status_, data_[0], need == 2 ? data_[1] : 0, link);
            have_ = 0;  // status stays: running status
        }
    }
}

void McuProtocol::challenge_response(const std::uint8_t c[4], std::uint8_t r[4]) {
    // The published formula; C precedence makes the extra parentheses explicit.
    const int c0 = c[0], c1 = c[1], c2 = c[2], c3 = c[3];
    r[0] = static_cast<std::uint8_t>(0x7F & (c0 + (c1 ^ 0x0A) - c3));
    r[1] = static_cast<std::uint8_t>(0x7F & ((c2 >> 4) ^ (c0 + c3)));
    r[2] = static_cast<std::uint8_t>(0x7F & ((c3 - (c2 << 2)) ^ (c0 | c1)));
    r[3] = static_cast<std::uint8_t>(0x7F & (c1 - c2 + (0xF0 ^ (c3 << 4))));
}

void McuProtocol::query_device() {
    const std::uint8_t m[] = {kSysexHeader[0], kSysexHeader[1], kSysexHeader[2],
                              kSysexHeader[3], kSysexHeader[4], kCmdDeviceQuery, 0xF7};
    out(m, sizeof m);
}

void McuProtocol::sysex(SurfaceLink&) {
    // sysex_ holds what followed F0: 00 00 66 14 <cmd> <params>.
    if (sysex_len_ < 5 || std::memcmp(sysex_, kSysexHeader + 1, 4) != 0) return;
    const std::uint8_t cmd = sysex_[4];
    const std::uint8_t* params = sysex_ + 5;
    const std::size_t n = sysex_len_ - 5;
    if (cmd == kCmdConnectionQuery && n == kSerialLen + kChallengeLen) {
        std::uint8_t m[sizeof kSysexHeader + 1 + kSerialLen + kChallengeLen + 1];
        std::size_t at = 0;
        for (const auto b : kSysexHeader) m[at++] = b;
        m[at++] = kCmdConnectionReply;
        for (std::size_t i = 0; i < kSerialLen; ++i) m[at++] = params[i];
        std::uint8_t resp[4];
        challenge_response(params + kSerialLen, resp);
        for (const auto b : resp) m[at++] = b;
        m[at++] = 0xF7;
        out(m, at);
    } else if (cmd == kCmdConnectionConfirm && n == kSerialLen) {
        online_ = true;
        refused_ = false;
        invalidate_page();
    } else if (cmd == kCmdConnectionError && n == kSerialLen) {
        online_ = false;
        refused_ = true;
    }
}

void McuProtocol::message(std::uint8_t status, std::uint8_t d1, std::uint8_t d2,
                          SurfaceLink& link) {
    const std::uint8_t kind = status & 0xF0;
    const std::uint8_t ch = status & 0x0F;
    if (kind == 0xE0) {
        if (ch >= kMcuStrips) return;  // the master fader has no strip in the contract
        const std::size_t strip = offset_ + ch;
        const int v = d1 | (d2 << 7);
        fader_[strip] = static_cast<float>(v) / static_cast<float>(kFaderMax);
        send_event(link, EventKind::Fader, static_cast<std::uint8_t>(strip), touched_[strip],
                   fader_[strip]);
    } else if (kind == 0x90) {
        note(d1, d2 > 0, link);
    } else if (kind == 0xB0) {
        const bool ccw = (d2 & 0x40) != 0;
        const int delta = d2 & 0x3F;
        if (d1 >= kCcVPot && d1 < kCcVPot + kMcuStrips) {
            const std::size_t strip = offset_ + (d1 - kCcVPot);
            const float step = cfg_.pan_step * static_cast<float>(delta) * (ccw ? -1.0f : 1.0f);
            pan_[strip] = std::clamp(pan_[strip] + step, -1.0f, 1.0f);
            send_event(link, EventKind::Pan, static_cast<std::uint8_t>(strip), false, pan_[strip]);
        } else if (d1 == kCcJog && delta > 0) {
            send_transport(link, ccw ? TransportCmd::Rewind : TransportCmd::Forward,
                           static_cast<float>(delta));
        }
    }
}

void McuProtocol::note(std::uint8_t n, bool on, SurfaceLink& link) {
    if (n >= kNoteTouch && n < kNoteTouch + kMcuStrips) {
        const std::size_t strip = offset_ + (n - kNoteTouch);
        touched_[strip] = on;
        if (!on) sent_fader_[strip - offset_] = -1;  // put the motor back on the engine's value
        send_event(link, EventKind::Fader, static_cast<std::uint8_t>(strip), on, fader_[strip]);
        return;
    }
    if (!on) return;  // buttons act on press
    const auto local = [&](std::uint8_t base) { return offset_ + (n - base); };
    if (n < kNoteSolo) {
        const std::size_t s = local(kNoteRec);
        send_event(link, EventKind::Arm, static_cast<std::uint8_t>(s), !arm_[s], 0);
    } else if (n < kNoteMute) {
        const std::size_t s = local(kNoteSolo);
        send_event(link, EventKind::Solo, static_cast<std::uint8_t>(s), !solo_[s], 0);
    } else if (n < kNoteSelect) {
        const std::size_t s = local(kNoteMute);
        send_event(link, EventKind::Mute, static_cast<std::uint8_t>(s), !mute_[s], 0);
    } else if (n < kNoteVPush) {
        send_event(link, EventKind::Select, static_cast<std::uint8_t>(local(kNoteSelect)), true, 0);
    } else if (n < kNoteVPush + kMcuStrips) {
        const std::size_t s = local(kNoteVPush);  // pushing the pot centres the pan
        pan_[s] = 0;
        send_event(link, EventKind::Pan, static_cast<std::uint8_t>(s), false, 0);
    } else if (n == kNoteBankLeft || n == kNoteBankRight || n == kNoteChannelLeft ||
               n == kNoteChannelRight) {
        const std::size_t top = kMaxStrips - kMcuStrips;
        std::size_t next = offset_;
        if (n == kNoteBankLeft) next = offset_ >= kMcuStrips ? offset_ - kMcuStrips : 0;
        if (n == kNoteBankRight) next = std::min(top, offset_ + kMcuStrips);
        if (n == kNoteChannelLeft) next = offset_ > 0 ? offset_ - 1 : 0;
        if (n == kNoteChannelRight) next = std::min(top, offset_ + 1);
        if (next != offset_) {
            offset_ = next;
            invalidate_page();
        }
    } else if (n == kNoteRewind) {
        send_transport(link, TransportCmd::Rewind, 0);
    } else if (n == kNoteForward) {
        send_transport(link, TransportCmd::Forward, 0);
    } else if (n == kNoteStop) {
        send_transport(link, TransportCmd::Stop, 0);
    } else if (n == kNotePlay) {
        send_transport(link, TransportCmd::Play, 0);
    } else if (n == kNoteRecord) {
        send_transport(link, TransportCmd::Record, 0);
    } else if (n == kNoteCycle) {
        send_transport(link, TransportCmd::Loop, 0);
    }
}

void McuProtocol::set_time_display(const char* text) {
    std::size_t at = 0;
    for (std::size_t i = 0; text[i] != '\0' && at < sizeof time_want_; ++i) {
        if (text[i] == '.' && at > 0) {
            time_want_[at - 1] = static_cast<char>(time_want_[at - 1] | 0x80);  // point flag
        } else {
            time_want_[at++] = text[i];
        }
    }
    for (; at < sizeof time_want_; ++at) time_want_[at] = ' ';
}

void McuProtocol::render(SurfaceLink& link) {
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
    if (cfg_.clock_ms) {
        const std::uint64_t now = cfg_.clock_ms();
        meters_due = !meter_started_ || now - last_meter_ms_ >= cfg_.meter_interval_ms;
        if (meters_due) {
            last_meter_ms_ = now;
            meter_started_ = true;
        }
    }

    for (std::size_t i = 0; i < kMcuStrips; ++i) {
        const std::size_t s = offset_ + i;
        const auto lane = static_cast<std::uint8_t>(i);

        // A finger on the fader owns it: the motor must not fight the hand.
        const int want_fader = fader_to_14(fader_[s]);
        if (!touched_[s] && want_fader != sent_fader_[i]) {
            const std::uint8_t m[] = {static_cast<std::uint8_t>(0xE0 | lane),
                                      static_cast<std::uint8_t>(want_fader & 0x7F),
                                      static_cast<std::uint8_t>(want_fader >> 7)};
            out(m, 3);
            sent_fader_[i] = want_fader;
        }

        const int pos = pan_position(pan_[s]);
        const int ring = pos | (pos == 6 ? 0x40 : 0);  // mode 0: single dot, centre LED lit
        if (ring != sent_ring_[i]) {
            const std::uint8_t m[] = {0xB0, static_cast<std::uint8_t>(kCcRing + lane),
                                      static_cast<std::uint8_t>(ring)};
            out(m, 3);
            sent_ring_[i] = ring;
        }

        const bool btn[3] = {arm_[s], solo_[s], mute_[s]};
        const std::uint8_t base[3] = {kNoteRec, kNoteSolo, kNoteMute};
        for (int b = 0; b < 3; ++b) {
            if (sent_button_[b][i] != (btn[b] ? 1 : 0)) {
                const std::uint8_t m[] = {0x90, static_cast<std::uint8_t>(base[b] + lane),
                                          static_cast<std::uint8_t>(btn[b] ? 0x7F : 0x00)};
                out(m, 3);
                sent_button_[b][i] = btn[b] ? 1 : 0;
            }
        }

        if (meters_due && level_[s] != sent_level_[i]) {
            const std::uint8_t m[] = {0xD0, static_cast<std::uint8_t>((lane << 4) | level_[s])};
            out(m, 2);
            sent_level_[i] = level_[s];
        }

        if (!sent_text_valid_[i] || std::memcmp(sent_text_[i], text_[s], kScribbleChars) != 0) {
            std::uint8_t m[sizeof kSysexHeader + 2 + kScribbleChars + 1];
            std::size_t n = 0;
            for (const auto b : kSysexHeader) m[n++] = b;
            m[n++] = kCmdLcd;
            m[n++] = static_cast<std::uint8_t>(i * kScribbleChars);  // upper line
            for (std::size_t c = 0; c < kScribbleChars; ++c) {
                const auto ch = static_cast<unsigned char>(text_[s][c]);
                m[n++] = (ch >= 0x20 && ch < 0x7F) ? ch : '?';
            }
            m[n++] = 0xF7;
            out(m, n);
            std::memcpy(sent_text_[i], text_[s], kScribbleChars);
            sent_text_valid_[i] = true;
        }
    }

    for (std::size_t t = 1; t < 7; ++t) {
        const auto c = static_cast<TransportCmd>(t);
        if (sent_lit_[t] != (lit_[t] ? 1 : 0)) {
            const std::uint8_t m[] = {0x90, note_for(c),
                                      static_cast<std::uint8_t>(lit_[t] ? 0x7F : 0x00)};
            out(m, 3);
            sent_lit_[t] = lit_[t] ? 1 : 0;
        }
    }

    // Digit 0 of the text is the leftmost; the device numbers digits right to left.
    for (std::size_t i = 0; i < sizeof time_want_; ++i) {
        const char c = static_cast<char>(time_want_[i] & 0x7F);
        const int code = time_code(c, (time_want_[i] & 0x80) != 0);
        if (code != time_sent_[i]) {
            const std::uint8_t m[] = {
                0xB0, static_cast<std::uint8_t>(kCcTime + (sizeof time_want_ - 1 - i)),
                static_cast<std::uint8_t>(code)};
            out(m, 3);
            time_sent_[i] = code;
        }
    }
}

void McuProtocol::poll(SurfaceLink& link) {
    std::uint8_t buf[256];
    for (;;) {
        const std::size_t n = port_.read(buf, sizeof buf);
        if (n == 0) break;
        on_midi(buf, n, link);
    }
    render(link);
}

}  // namespace sonora::surfaces
