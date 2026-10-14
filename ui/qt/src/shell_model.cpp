// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell_model.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QVariantMap>
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
    peaks_.assign(engine_.info().track_count, 0.0f);
    peak_read_.assign(engine_.info().track_count, false);
    replies_.reserve(16);
    events_.reserve(16);
    xkb_scan_codes_ = qt_scan_codes_are_xkb(QGuiApplication::platformName());
    seen_focus_ = router_.commandsFocus();
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

void ShellModel::setKeymapNotices(const QStringList& profile, const QStringList& essentials) {
    for (const QString& old : keymap_notices_) notices_.removeOne(old);
    profile_notices_ = profile;
    essentials_notices_ = essentials;
    keymap_notices_ = profile + essentials;
    notices_ << keymap_notices_;
    emit noticesChanged();
}

void ShellModel::followEngineCommands() {
    const std::vector<std::string> notices =
        router_.setEngineCommands(engine_.info().implemented_commands);
    QStringList lines;
    for (const auto& n : notices) lines << QString::fromStdString(n);
    if (lines != essentials_notices_) setKeymapNotices(profile_notices_, lines);
    seen_focus_ = router_.commandsFocus();
    emit keymapChanged();
}

void ShellModel::setProfiles(std::vector<ProfileEntry> entries, std::string current,
                             Remember remember) {
    profiles_ = std::move(entries);
    // Compared with each entry's file, which list_profiles normalised.
    profile_file_ = profile_path(current);
    remember_ = std::move(remember);
    emit keymapChanged();
}

QVariantList ShellModel::profiles() const {
    QVariantList out;
    for (const ProfileEntry& e : profiles_) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), QString::fromStdString(e.name));
        m.insert(QStringLiteral("file"), QString::fromStdString(e.file));
        m.insert(QStringLiteral("error"), QString::fromStdString(e.error));
        m.insert(QStringLiteral("current"), e.file == profile_file_);
        out << m;
    }
    return out;
}

void ShellModel::useProfile(const QString& qfile) {
    const std::string file = profile_path(qfile.toStdString());
    // Loaded again, not taken from the list: the file may have changed
    // since the list was made, and what runs must be what was checked.
    KeyMap map;
    std::string err;
    if (file.empty()) {
        map = KeyMap::built_in_default();
    } else if (!KeyMap::load_file(file, map, err)) {
        for (ProfileEntry& e : profiles_) {
            if (e.file == file) e.error = err;
        }
        emit keymapChanged();
        say(QStringLiteral("key map refused, keys unchanged: ") + QString::fromStdString(err));
        return;
    }
    const std::vector<std::string> notices = router_.setMap(std::move(map));
    profile_file_ = file;
    for (ProfileEntry& e : profiles_) {
        if (e.file == file) e.error.clear();
    }
    QStringList essentials;
    for (const auto& n : notices) essentials << QString::fromStdString(n);
    QStringList lines;
    std::string why;
    if (remember_ && !remember_(file, why)) {
        lines << QStringLiteral("this key map is used until the interface restarts, and "
                                "then the previous one again: ") +
                     QString::fromStdString(why);
    }
    setKeymapNotices(lines, essentials);
    seen_focus_ = router_.commandsFocus();
    emit keymapChanged();
    say(QStringLiteral("Keys: ") + profileName());
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
    if (text_entry_) {
        switch (event->type()) {
            case QEvent::ShortcutOverride:
            case QEvent::KeyPress:
            case QEvent::KeyRelease:
                return QObject::eventFilter(watched, event);
            default:
                break;
        }
    }
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
            switch (chord_from_qt_key(qt_key_input(*k, xkb_scan_codes_), chord)) {
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
    if (router_.commandsFocus() != seen_focus_) {
        seen_focus_ = router_.commandsFocus();
        emit keymapChanged();
    }
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
    // A handful of short strings, compared without allocating; they differ
    // only after a reconnect to an engine that is another build.
    if (engine_.info().implemented_commands != router_.engineCommands()) followEngineCommands();
    for (const auto& r : replies_) {
        if (r.result == DispatchResult::Ok) {
            if (!r.live && !status_.isEmpty()) say(QString());
            continue;
        }
        const std::string why = r.message.empty() ? dispatch_result_text(r.result) : r.message;
        say(QString::fromStdString(r.command + ": " + why));
    }
    takeEvents();
    if (engine_.take_catalog(catalog_) && plugin_sinks_.catalog) plugin_sinks_.catalog(catalog_);

    const bool alive = engine_.engine_alive(now_ns_());
    if (alive != alive_) {
        alive_ = alive;
        emit engineAliveChanged();
    }
    std::fill(peak_read_.begin(), peak_read_.end(), false);
    for (MeterItem* m : meters_) {
        if (!alive) {
            m->setNoData();
            continue;
        }
        if (!m->isVisible()) continue;
        const auto ch = static_cast<std::size_t>(m->channel());
        if (ch >= peaks_.size()) continue;
        if (!peak_read_[ch]) {
            peaks_[ch] = engine_.take_peak(static_cast<uint32_t>(ch));
            peak_read_[ch] = true;
        }
        m->feed(peaks_[ch], dt);
    }
    // One frame of the state block per tick at most, shared by the sink
    // and the refresh, so both draw the same publish.
    const bool refresh = ++tick_count_ % kStateEvery == 0;
    if (alive && (transport_sink_ || refresh)) engine_.frame(frame_);
    if (transport_sink_) transport_sink_(alive ? frame_.transport : TransportSnapshot{}, alive);
    if (refresh) refreshState(false);
    if (refresh && plugin_sinks_.frame) plugin_sinks_.frame(frame_, alive);
    if (!status_.isEmpty() && t0 > status_until_ns_) {
        status_.clear();
        emit statusChanged();
    }

    if (stats_ && stats_->tick_ns.size() < stats_->tick_ns.capacity()) {
        stats_->tick_ns.push_back(clock_.nsecsElapsed() - t0);
    }
}

void ShellModel::takeEvents() {
    events_.clear();
    engine_.take_events(events_);
    if (events_.empty()) return;
    for (const EngineEvent& e : events_) {
        const QString text = QString::fromStdString(e.text);
        switch (e.kind) {
            case EngineEventKind::Notice:
                addNotice(text);
                break;
            case EngineEventKind::InputsMissing:
                inputs_missing_ = text;
                break;
            case EngineEventKind::TakeFailed:
                take_failed_ = text;
                break;
            case EngineEventKind::Fatal:
                fatal_ = text.isEmpty() ? QStringLiteral("the audio backend failed") : text;
                break;
            case EngineEventKind::PluginFault:
                if (plugin_sinks_.fault) {
                    plugin_sinks_.fault(e.insert, e.text);
                } else if (!e.text.empty()) {
                    // No slot to show it on: a notice, so it is not lost.
                    addNotice(text);
                }
                break;
        }
    }
    // Most urgent first: a backend that stopped ends the session, a failed
    // take is losing audio now, missing inputs record silence.
    QStringList parts;
    if (!fatal_.isEmpty()) parts << fatal_;
    if (!take_failed_.isEmpty()) parts << take_failed_;
    if (!inputs_missing_.isEmpty()) parts << QStringLiteral("no inputs: ") + inputs_missing_;
    const QString alert = parts.join(QStringLiteral(" · "));
    if (alert != alert_text_) {
        alert_text_ = alert;
        emit alertChanged();
    }
}

void ShellModel::refreshState(bool read_frame) {
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

    if (read_frame) engine_.frame(frame_);
    const TransportSnapshot& t = frame_.transport;
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
        if (t.health_flags & kHealthTakeFailed) health << QStringLiteral("take failed");
        if (t.health_flags & kHealthPluginsDisabled) health << QStringLiteral("plugins disabled");
        set(health_text_, health.join(QStringLiteral(" · ")));
    }

    QList<bool> armed;
    int overruns = 0;
    for (uint32_t i = 0; i < engine_.info().track_count; ++i) {
        const TrackSnapshot s = i < frame_.tracks.size() ? frame_.tracks[i] : TrackSnapshot{};
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
