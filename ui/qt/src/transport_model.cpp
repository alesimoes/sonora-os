// SPDX-License-Identifier: GPL-3.0-or-later
#include "transport_model.h"

#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <utility>

#include "session_model.h"
#include "shell_model.h"
#include "sonora/command.h"

namespace sonora::ui {

namespace {

// As SessionModel's counters: every fourth tick while rolling (15 Hz at
// 60), which is faster than anyone reads a number and a quarter of the
// text layouts.
constexpr unsigned kEvery = 4;

QString q(const std::string& s) { return QString::fromStdString(s); }

bool format_named(const QString& name, TimeFormat& out) {
    for (TimeFormat f : {TimeFormat::BarsBeats, TimeFormat::MinSec, TimeFormat::Timecode,
                         TimeFormat::Samples}) {
        if (name == QLatin1String(time_format_name(f))) {
            out = f;
            return true;
        }
    }
    return false;
}

// A length, not a position: "2.000 s". Pre-roll reads the same at any bar.
QString duration_text(model::Frames f, uint32_t rate) {
    if (rate == 0) return QStringLiteral("--");
    return QStringLiteral("%1 s").arg(static_cast<double>(f) / rate, 0, 'f', 3);
}

}  // namespace

TransportModel::TransportModel(Settings settings, Insertion insertion, QObject* parent)
    : QObject(parent), settings_fn_(std::move(settings)), insertion_fn_(std::move(insertion)) {
    settings_ = settings_fn_();
    refreshTempo(true);
    position_ = insertion_fn_();
    refreshPosition(true);
}

void TransportModel::installCommands(CommandRegistry& local) {
    local.install("window.transport", [this](const CommandContext&) {
        setOpen(!open_);
        return true;
    });
}

void TransportModel::setOpen(bool on) {
    if (on == open_) return;
    open_ = on;
    emit openChanged();
}

void TransportModel::onTransport(const TransportSnapshot& t, bool alive) {
    const model::SessionSettings& now = settings_fn_();
    bool settings_changed = false;
    if (!(now == settings_)) {
        const bool rate_changed = now.sample_rate != settings_.sample_rate;
        const bool tempo_changed = now.tempo != settings_.tempo || rate_changed;
        const bool timecode_changed = !(now.timecode == settings_.timecode);
        settings_changed = !(now.transport == settings_.transport) || rate_changed ||
                           timecode_changed;
        settings_ = now;
        if (tempo_changed) refreshTempo(true);
        // The same position reads differently now: what is shown is stale
        // even though the position did not move.
        if (tempo_changed || timecode_changed) shown_ = -1;
    }

    // A stale read keeps what was shown; it is one tick's seqlock miss, and
    // the transport has not stopped because a read failed (ADR-0007 §3).
    const bool state_known = !alive || !t.stale;
    bool state_changed = false;
    if (state_known) {
        const bool rolling = alive && t.state != TransportMode::Stopped;
        const bool recording = rolling && t.state == TransportMode::Recording;
        const QString text = !alive                 ? QStringLiteral("NO ENGINE")
                             : recording            ? QStringLiteral("REC")
                             : rolling              ? QStringLiteral("PLAY")
                                                    : QStringLiteral("STOP");
        const bool click = alive && t.click;
        const bool count_off = alive && t.count_off;
        const bool counting_off = alive && t.counting_off;
        state_changed = rolling != rolling_ || recording != recording_ || alive != alive_ ||
                        text != state_text_ || click != click_ || count_off != count_off_ ||
                        counting_off != counting_off_;
        click_ = click;
        count_off_ = count_off;
        counting_off_ = counting_off;
        rolling_ = rolling;
        recording_ = recording;
        alive_ = alive;
        state_text_ = text;
    }
    // A dead engine's playhead is not current; the insertion point is ours.
    if (rolling_ && state_known) {
        position_ = static_cast<model::Frames>(t.playhead_frames);
    } else if (!rolling_) {
        position_ = insertion_fn_();
    }

    if (state_changed) emit stateChanged();
    if (settings_changed) emit settingsChanged();
    // Following the main counter, a rolling position is published on its
    // cue alone (followCounter), so the two never show different readings.
    const bool force = !rolling_ ||
                       (!counter_led_ && (state_changed || settings_changed ||
                                          ++ticks_ % kEvery == 0));
    refreshPosition(force);
    refreshTempo(false);
}

void TransportModel::followCounter() {
    if (rolling_) refreshPosition(true);
}

void TransportModel::refreshPosition(bool force) {
    if (!force || position_ == shown_) return;
    shown_ = position_;
    bars_beats_ = q(format_bars_beats(position_, settings_));
    timecode_ = q(format_timecode(position_, settings_));
    min_secs_ = q(format_min_sec(position_, settings_.sample_rate));
    samples_ = q(format_samples(position_));
    emit positionChanged();
}

void TransportModel::refreshTempo(bool force) {
    const std::size_t index = tempo_index_at(std::max<model::Frames>(0, position_), settings_);
    if (!force && index == tempo_index_) return;
    tempo_index_ = index;
    const model::TempoEntry& now = tempo_at(std::max<model::Frames>(0, position_), settings_);
    tempo_text_ = q(format_bpm(now.bpm));
    meter_text_ = q(format_meter(now));
    // One map for the whole list: a reading per entry, each building its
    // own, was quadratic in the map's length.
    const std::vector<std::string> at = tempo_entry_positions(settings_);
    next_change_.clear();
    if (index + 1 < settings_.tempo.size()) {
        const model::TempoEntry& next = settings_.tempo[index + 1];
        next_change_ = QStringLiteral("%1  %2 at %3")
                           .arg(q(format_bpm(next.bpm)), q(format_meter(next)), q(at[index + 1]));
    }
    tempo_map_.clear();
    for (std::size_t i = 0; i < settings_.tempo.size(); ++i) {
        const model::TempoEntry& e = settings_.tempo[i];
        QVariantMap m;
        m.insert(QStringLiteral("at"), q(at[i]));
        m.insert(QStringLiteral("bpm"), q(format_bpm(e.bpm)));
        m.insert(QStringLiteral("meter"), q(format_meter(e)));
        m.insert(QStringLiteral("current"), i == index);
        tempo_map_.push_back(m);
    }
    emit tempoChanged();
}

void link_transport(ShellModel& shell, SessionModel& session, TransportModel& transport) {
    transport.counter_led_ = true;
    // The window takes the playhead first, so that when SessionModel
    // publishes its counters in the same tick the window publishes the same
    // position with them.
    shell.setTransportSink([&session, &transport](const TransportSnapshot& t, bool alive) {
        transport.onTransport(t, alive);
        session.onTransport(t, alive);
    });
    QObject::connect(&session, &SessionModel::counterChanged, &transport,
                     &TransportModel::followCounter);
}

void link_marker_locate(ShellModel& shell, SessionModel& session) {
    session.controller().set_locator([&shell](model::Frames frame) {
        shell.invokeWith(QStringLiteral("transport.locate"), -1, QString::number(frame));
    });
}

QString TransportModel::positionSource() const {
    if (!alive_) return QStringLiteral("insertion point (no engine)");
    return rolling_ ? QStringLiteral("playhead") : QStringLiteral("insertion point");
}

QString TransportModel::preRollText() const {
    return duration_text(settings_.transport.pre_roll, settings_.sample_rate);
}

QString TransportModel::postRollText() const {
    return duration_text(settings_.transport.post_roll, settings_.sample_rate);
}

QString TransportModel::rollsWhy() {
    return QStringLiteral(
        "Pre-roll and post-roll run around a punch, a loop take and a played selection; a "
        "plain take starts at the playhead. Click a value to type seconds.");
}

bool parse_roll_text(const std::string& text, uint32_t rate, model::Frames& frames,
                     std::string& why) {
    std::string s;
    for (const char c : text) {
        if (c != ' ' && c != '	') s += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    if (!s.empty() && s.back() == 's') s.pop_back();
    // Digits with at most one point, at least one digit: no sign, so a
    // negative roll is refused as text rather than read as zero.
    double seconds = 0.0;
    double scale = 0.0;
    bool digit = false;
    for (const char c : s) {
        if (c == '.' && scale == 0.0) {
            scale = 0.1;
        } else if (c >= '0' && c <= '9') {
            digit = true;
            if (scale == 0.0) {
                seconds = seconds * 10 + (c - '0');
            } else {
                seconds += scale * (c - '0');
                scale /= 10;
            }
            if (seconds > kMaxRollSeconds) break;
        } else {
            digit = false;
            break;
        }
    }
    if (!digit || rate == 0) {
        why = "\"" + text + "\" is not a length; type seconds, like 2 or 0.5";
        return false;
    }
    if (seconds > kMaxRollSeconds) {
        why = "a roll goes from 0 to " + std::to_string(kMaxRollSeconds) + " seconds; \"" + text +
              "\" is past that";
        return false;
    }
    frames = static_cast<model::Frames>(std::llround(seconds * rate));
    return true;
}

QString TransportModel::timeText(qint64 frames, const QString& format) const {
    TimeFormat f;
    if (!format_named(format, f)) return QStringLiteral("?");
    return q(format_time(f, frames, settings_));
}

QString TransportModel::rangeText(qint64 start, qint64 end, const QString& format) const {
    if (end <= start) return QStringLiteral("not set");
    return timeText(start, format) + QStringLiteral(" – ") + timeText(end, format);
}

}  // namespace sonora::ui
