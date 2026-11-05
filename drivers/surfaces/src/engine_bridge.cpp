// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/surfaces/engine_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "sonora/api/state_block.h"

namespace sonora::surfaces {

namespace {

struct TaperPoint {
    float db;
    float pos;
};
constexpr float kSilenceDb = -144.0f;
constexpr TaperPoint kTaper[] = {
    {kSilenceDb, 0.00f}, {-60.0f, 0.08f}, {-40.0f, 0.20f}, {-30.0f, 0.30f}, {-20.0f, 0.42f},
    {-10.0f, 0.58f},     {-5.0f, 0.68f},  {0.0f, 0.78f},   {6.0f, 0.90f},   {12.0f, 1.00f},
};
constexpr int kPoints = sizeof kTaper / sizeof kTaper[0];

float db_from_position(float p) {
    if (!(p > 0.0f)) return kSilenceDb;
    if (p >= 1.0f) return kTaper[kPoints - 1].db;
    for (int i = 1; i < kPoints; ++i) {
        if (p <= kTaper[i].pos) {
            const TaperPoint& a = kTaper[i - 1];
            const TaperPoint& b = kTaper[i];
            return a.db + (p - a.pos) / (b.pos - a.pos) * (b.db - a.db);
        }
    }
    return kTaper[kPoints - 1].db;
}

float position_from_db(float db) {
    if (!(db > kTaper[0].db)) return 0.0f;
    if (db >= kTaper[kPoints - 1].db) return 1.0f;
    for (int i = 1; i < kPoints; ++i) {
        if (db <= kTaper[i].db) {
            const TaperPoint& a = kTaper[i - 1];
            const TaperPoint& b = kTaper[i];
            return a.pos + (db - a.db) / (b.db - a.db) * (b.pos - a.pos);
        }
    }
    return 1.0f;
}

// A peak as a bar: -48 dBFS is empty, 0 dBFS is full.
float meter_position(float peak) {
    if (!(peak > 0.0f)) return 0.0f;
    return std::clamp((20.0f * std::log10(peak) + 48.0f) / 48.0f, 0.0f, 1.0f);
}

const char* transport_command(TransportCmd c) {
    switch (c) {
        case TransportCmd::Play: return "transport.play";
        case TransportCmd::Stop: return "transport.stop";
        case TransportCmd::Record: return "transport.record";
        case TransportCmd::Rewind: return "transport.rewind";
        case TransportCmd::Forward: return "transport.fast_forward";
        case TransportCmd::Loop: return "transport.toggle_loop";
        default: return nullptr;
    }
}

constexpr int kMaxJogCommands = 4;  // there is no scrub command; a spin is a few steps

}  // namespace

float surface_gain_from_position(float position) {
    const float db = db_from_position(position);
    if (!(db > kSilenceDb)) return 0.0f;
    return std::min(4.0f, std::pow(10.0f, db / 20.0f));
}

float surface_position_from_gain(float gain) {
    if (!(gain > 0.0f)) return 0.0f;
    return position_from_db(20.0f * std::log10(gain));
}

void EngineSurfaceBridge::command(const char* id, int track) {
    api::CommandRequest r;
    r.command = id;
    r.target = track;
    engine_.send(r);
}

void EngineSurfaceBridge::drain(SurfaceLink& link) {
    SurfaceEvent e{};
    while (link.pop_event(e)) {
        if (e.kind == EventKind::Transport) {
            const char* id = transport_command(e.transport);
            if (id == nullptr) continue;
            // A jog wheel carries its detent count; a button carries 0.
            const int times = e.value > 0.0f
                                  ? std::min(kMaxJogCommands, static_cast<int>(e.value))
                                  : 1;
            for (int i = 0; i < times; ++i) command(id, -1);
            continue;
        }
        if (e.strip >= kMaxStrips) continue;
        Strip& s = strips_[e.strip];
        switch (e.kind) {
            case EventKind::Fader: {
                api::ParameterChange c;
                c.track = e.strip;
                c.param = api::ParamId::Gain;
                c.value = surface_gain_from_position(e.value);
                if (e.flag && !s.touched) c.gesture = api::Gesture::Touch;
                else if (!e.flag && s.touched) c.gesture = api::Gesture::Release;
                else c.gesture = api::Gesture::Move;
                s.touched = e.flag;
                s.fader = e.value;  // the engine's echo of this move is not news
                engine_.send(c);
                break;
            }
            case EventKind::Pan: {
                api::ParameterChange c;
                c.track = e.strip;
                c.param = api::ParamId::Pan;
                c.value = e.value;
                engine_.send(c);
                break;
            }
            case EventKind::Mute:
                if (e.flag != s.mute) {
                    s.mute = e.flag;
                    command("track.toggle_mute", e.strip);
                }
                break;
            case EventKind::Solo:
                if (e.flag != s.solo) {
                    s.solo = e.flag;
                    command("track.toggle_solo", e.strip);
                }
                break;
            case EventKind::Arm:
                if (e.flag != s.arm) {
                    s.arm = e.flag;
                    command("track.toggle_arm", e.strip);
                }
                break;
            case EventKind::Select:
                if (e.flag) command("track.select", e.strip);
                break;
            default: break;
        }
    }
}

void EngineSurfaceBridge::publish(SurfaceLink& link, const api::StateFrame& frame,
                                  const PeakFn& peak, const std::vector<std::string>& names) {
    const std::size_t count = std::min<std::size_t>(frame.tracks.size(), kMaxStrips);
    for (std::size_t i = 0; i < count; ++i) {
        const api::TrackSnapshot& t = frame.tracks[i];
        Strip& s = strips_[i];
        const auto strip = static_cast<std::uint8_t>(i);
        SurfaceFeedback f{};
        f.strip = strip;

        const float pos = surface_position_from_gain(t.gain);
        if (!s.touched && std::fabs(pos - s.fader) > 0.001f) {
            f.kind = FeedbackKind::Fader;
            f.value = pos;
            link.push_feedback(f);
            s.fader = pos;
        }
        const struct {
            FeedbackKind kind;
            bool now;
            bool* mine;
        } flags[] = {
            {FeedbackKind::Mute, t.muted(), &s.mute},
            {FeedbackKind::Solo, (t.flags & api::kTrackSolo) != 0, &s.solo},
            {FeedbackKind::Arm, t.armed(), &s.arm},
        };
        for (const auto& fl : flags) {
            if (*fl.mine != fl.now || !s.named) {
                SurfaceFeedback b{};
                b.kind = fl.kind;
                b.strip = strip;
                b.flag = fl.now;
                if (link.push_feedback(b)) *fl.mine = fl.now;
            }
        }
        if (peak) {
            const float v = meter_position(peak(static_cast<std::uint32_t>(i)));
            const int level = static_cast<int>(std::lround(v * 12.0f));
            if (level != s.level) {
                SurfaceFeedback m{};
                m.kind = FeedbackKind::Meter;
                m.strip = strip;
                m.value = v;
                if (link.push_feedback(m)) s.level = level;
            }
        }
        std::string want;
        if (i < names.size() && !names[i].empty()) {
            want = names[i];
        } else {
            char buf[16];
            std::snprintf(buf, sizeof buf, "Trk %02zu", i + 1);
            want = buf;
        }
        if (!s.named || s.name != want) {
            SurfaceFeedback sc{};
            sc.kind = FeedbackKind::Scribble;
            sc.strip = strip;
            sc.set_text(want.c_str());
            if (link.push_feedback(sc)) {
                s.name = want;
                s.named = true;
            }
        }
    }

    const bool lit[7] = {false,
                         frame.transport.state == api::TransportState::Rolling ||
                             frame.transport.state == api::TransportState::Recording,
                         frame.transport.state == api::TransportState::Stopped,
                         frame.transport.state == api::TransportState::Recording,
                         false, false, false};
    for (int c = 1; c <= 3; ++c) {  // Play, Stop, Record
        if (lit_[c] != (lit[c] ? 1 : 0)) {
            SurfaceFeedback t{};
            t.kind = FeedbackKind::Transport;
            t.transport = static_cast<TransportCmd>(c);
            t.flag = lit[c];
            if (link.push_feedback(t)) lit_[c] = lit[c] ? 1 : 0;
        }
    }
}

}  // namespace sonora::surfaces
