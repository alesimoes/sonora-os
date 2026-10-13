// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell_model.h"

#include <QKeyEvent>
#include <QQuickWindow>
#include <algorithm>
#include <chrono>

#include "meter_item.h"
#include "meter_model.h"
#include "qt_keys.h"

namespace sonora::ui {

namespace {

// The transport text and track flags change at human rates. Re-reading them
// every fourth tick (15 Hz) costs a quarter of the text layout and still
// moves the clock faster than anyone reads it.
constexpr unsigned kStateEvery = 4;
// How long a status message stays up, as at the tty: long enough to read,
// short enough not to become furniture.
constexpr int64_t kStatusNs = 2'500'000'000;

QString clock_text(uint64_t frames, uint32_t rate) {
    if (rate == 0) return QStringLiteral("--:--:--.---");
    const uint64_t ms = frames * 1000 / rate;
    return QString::asprintf("%02llu:%02llu:%02llu.%03llu",
                             static_cast<unsigned long long>(ms / 3'600'000),
                             static_cast<unsigned long long>(ms / 60'000 % 60),
                             static_cast<unsigned long long>(ms / 1000 % 60),
                             static_cast<unsigned long long>(ms % 1000));
}

}  // namespace

ShellModel::ShellModel(EngineAdapter& engine, KeyRouter& router, QObject* parent)
    : QObject(parent), engine_(engine), router_(router), now_ns_([] {
          return static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count());
      }) {
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &ShellModel::tick);
    clock_.start();
    for (uint32_t i = 0; i < engine_.info().track_count; ++i) armed_.push_back(false);
    replies_.reserve(16);
}

void ShellModel::start(int hz) {
    setMeterHz(hz);
    refreshState();
    timer_.start();
}

void ShellModel::setMeterHz(int hz) {
    hz_ = std::clamp(hz, 1, 120);
    // Whole milliseconds: 60 Hz runs at 16 ms, i.e. 62.5 Hz. Close enough for
    // a display, and a precise timer cannot do fractions anyway.
    timer_.setInterval(std::max(1, 1000 / hz_));
    emit renderPathChanged();
}

QString ShellModel::formatText() const {
    const auto& i = engine_.info();
    return QStringLiteral("%1 Hz / %2 frames").arg(i.sample_rate).arg(i.buffer_frames);
}

QString ShellModel::profileName() const {
    return QString::fromStdString(router_.map().name());
}

QStringList ShellModel::helpEntries() const {
    QStringList out;
    for (const auto& h : router_.help()) out << QString::fromStdString(h);
    return out;
}

void ShellModel::addNotice(const QString& n) {
    notices_ << n;
    emit noticesChanged();
}

void ShellModel::setRenderPath(const QString& text, bool software, bool meters_available) {
    render_path_ = text;
    software_ = software;
    meters_available_ = meters_available;
    emit renderPathChanged();
}

void ShellModel::attachMeter(MeterItem* m) {
    if (std::find(meters_.begin(), meters_.end(), m) == meters_.end()) meters_.push_back(m);
}

void ShellModel::detachMeter(MeterItem* m) {
    meters_.erase(std::remove(meters_.begin(), meters_.end(), m), meters_.end());
}

void ShellModel::hideMeters() {
    for (MeterItem* m : meters_) m->setVisible(false);
}

void ShellModel::routeKeysFrom(QQuickWindow* window) { window->installEventFilter(this); }

bool ShellModel::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
        case QEvent::ShortcutOverride:
            // Claim every key before Qt's shortcut machinery or a focused
            // control can: there is one key system and it is the profile.
            event->accept();
            return true;
        case QEvent::KeyRelease:
            return true;
        case QEvent::KeyPress: {
            auto* k = static_cast<QKeyEvent*>(event);
            KeyChord chord;
            switch (chord_from_qt_key(k->key(), k->modifiers(), k->isAutoRepeat(), chord)) {
                case QtKeyDecode::Chord:
                    report(router_.route(chord));
                    break;
                case QtKeyDecode::Unrecognised:
                    say(QStringLiteral("that key means nothing here"));
                    break;
                case QtKeyDecode::ModifierOnly:
                case QtKeyDecode::AutoRepeat:
                    break;
            }
            return true;
        }
        default:
            return QObject::eventFilter(watched, event);
    }
}

void ShellModel::invoke(const QString& command, int target) {
    CommandContext ctx;
    ctx.target = target;
    report(router_.invoke(command.toStdString(), ctx));
}

qreal ShellModel::scaleY(qreal db, qreal h) const {
    const float top = meter_over_band(static_cast<float>(h)) + 1.0f;
    const float span = static_cast<float>(h) - top;
    return top + span * (1.0f - dbfs_to_fraction(static_cast<float>(db)));
}

QList<qreal> ShellModel::scaleMarks() const {
    QList<qreal> out;
    for (float db : kScaleMarksDb) out << db;
    return out;
}

void ShellModel::report(const RouteResult& r) {
    if (!r.message.empty()) {
        say(QString::fromStdString(r.message));
    } else if (r.kind == RouteResult::Kind::Local) {
        say(QString());
    }
    // A Sent command reports when its reply arrives, in tick().
}

void ShellModel::say(const QString& message) {
    status_until_ns_ = clock_.nsecsElapsed() + kStatusNs;
    if (status_ == message) return;
    status_ = message;
    emit statusChanged();
}

void ShellModel::tick() {
    const int64_t t0 = clock_.nsecsElapsed();
    const float dt = last_tick_ns_ < 0 ? 0.0f
                                       : static_cast<float>(t0 - last_tick_ns_) * 1e-9f;
    if (stats_ && last_tick_ns_ >= 0 && stats_->interval_ns.size() < stats_->interval_ns.capacity()) {
        stats_->interval_ns.push_back(t0 - last_tick_ns_);
    }
    last_tick_ns_ = t0;

    replies_.clear();
    engine_.poll(replies_);
    for (const auto& r : replies_) {
        if (r.result == DispatchResult::Ok) {
            if (!status_.isEmpty()) say(QString());
            continue;
        }
        const std::string why = r.message.empty() ? dispatch_result_text(r.result) : r.message;
        say(QString::fromStdString(r.command + ": " + why));
    }

    const bool alive = engine_.engine_alive(now_ns_());
    if (alive != alive_) {
        alive_ = alive;
        emit engineAliveChanged();
    }
    for (MeterItem* m : meters_) {
        if (!alive) {
            m->setNoData();
            continue;
        }
        if (!m->isVisible()) continue;
        m->feed(engine_.take_peak(static_cast<uint32_t>(m->channel())), dt);
    }

    if (++tick_count_ % kStateEvery == 0) refreshState();
    if (!status_.isEmpty() && t0 > status_until_ns_) {
        status_.clear();
        emit statusChanged();
    }

    if (stats_ && stats_->tick_ns.size() < stats_->tick_ns.capacity()) {
        stats_->tick_ns.push_back(clock_.nsecsElapsed() - t0);
    }
}

void ShellModel::refreshState() {
    bool changed = false;
    const auto set = [&changed](auto& field, const auto& value) {
        if (field != value) {
            field = value;
            changed = true;
        }
    };

    if (!alive_) {
        // Not the last values: a dead engine's last clock and take number
        // look exactly like a live one that is stopped.
        set(transport_text_, QStringLiteral("NO ENGINE"));
        set(recording_, false);
        set(clock_text_, QStringLiteral("--:--:--.---"));
        set(stale_, false);
        set(health_text_, QStringLiteral("engine not responding: meters show no data"));
        if (changed) emit transportChanged();
        return;
    }

    const TransportSnapshot t = engine_.transport();
    set(stale_, t.stale);
    if (!t.stale) {
        const QString mode = t.state == TransportMode::Recording ? QStringLiteral("REC")
                             : t.state == TransportMode::Rolling ? QStringLiteral("PLAY")
                                                                 : QStringLiteral("STOP");
        set(transport_text_, mode);
        set(recording_, t.state == TransportMode::Recording);
        set(clock_text_, clock_text(t.playhead_frames, engine_.info().sample_rate));
        set(take_, static_cast<int>(t.take));
        set(xruns_, static_cast<int>(t.xruns));
        QStringList health;
        if (t.health_flags & kHealthServerGone) health << QStringLiteral("audio server gone");
        if (t.health_flags & kHealthNoInputs) health << QStringLiteral("no inputs connected");
        set(health_text_, health.join(QStringLiteral(" · ")));
    }

    QList<bool> armed;
    int overruns = 0;
    for (uint32_t i = 0; i < engine_.info().track_count; ++i) {
        const TrackSnapshot s = engine_.track(i);
        armed << s.armed;
        overruns += static_cast<int>(s.overruns);
    }
    set(overruns_, overruns);
    if (changed) emit transportChanged();
    if (armed != armed_) {
        armed_ = armed;
        emit tracksChanged();
    }
}

}  // namespace sonora::ui
