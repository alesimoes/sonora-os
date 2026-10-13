// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_model.h"

#include <QVariantMap>
#include <algorithm>
#include <cmath>

#include "fader_taper.h"
#include "scene_item.h"
#include "session_files.h"
#include "sonora/command.h"
#include "sonora/model/session_file.h"

namespace sonora::ui {

using namespace model;

namespace {

// The counters move with the playhead; re-publishing their text every
// fourth tick (15 Hz) is the shell's state rate and faster than anyone
// reads a counter.
constexpr unsigned kCounterEvery = 4;

QString q(const std::string& s) { return QString::fromStdString(s); }

bool same(const Selection& a, const Selection& b) {
    return a.track == b.track && a.region == b.region && a.insertion == b.insertion &&
           a.start == b.start && a.end == b.end;
}

bool same(const ViewState& a, const ViewState& b) {
    return a.fpp == b.fpp && a.scroll == b.scroll && a.scroll_y == b.scroll_y &&
           a.width == b.width && a.height == b.height && a.track_height == b.track_height &&
           a.wave_scale == b.wave_scale;
}

bool same(const Drag& a, const Drag& b) {
    return a.kind == b.kind && a.moved == b.moved && a.start == b.start && a.end == b.end &&
           a.region == b.region;
}

}  // namespace

SessionModel::SessionModel(TimelineDocument& doc, const PeakSource& peaks, bool synthetic_peaks,
                           QObject* parent)
    : QObject(parent),
      doc_(doc),
      peaks_(peaks),
      synthetic_peaks_(synthetic_peaks),
      sessions_dir_(default_sessions_dir()),
      saved_revision_(doc.revision()),
      ctl_(doc) {
    updateCounters(true);
}

void SessionModel::installCommands(CommandRegistry& local) {
    using Why = std::string;
    const auto simple = [this, &local](const char* id, auto fn) {
        local.install(id, ExplainingHandler([this, fn](const CommandContext& ctx, Why& why) {
                          const bool ok = fn(ctx, why);
                          sync();
                          return ok;
                      }));
    };
    const auto mode = [this](EditMode m) {
        return [this, m](const CommandContext&, Why&) {
            ctl_.set_mode(m);
            return true;
        };
    };
    simple("edit.mode.shuffle", mode(EditMode::Shuffle));
    simple("edit.mode.slip", mode(EditMode::Slip));
    simple("edit.mode.spot", mode(EditMode::Spot));
    simple("edit.mode.grid", mode(EditMode::Grid));
    simple("edit.mode.next", [this](const CommandContext&, Why&) {
        ctl_.next_mode();
        return true;
    });

    const auto tool = [this](Tool t) {
        return [this, t](const CommandContext&, Why&) {
            ctl_.set_tool(t);
            return true;
        };
    };
    simple("tool.smart", tool(Tool::Smart));
    simple("tool.trim", tool(Tool::Trim));
    simple("tool.select", tool(Tool::Select));
    simple("tool.grab", tool(Tool::Grab));
    simple("tool.next", [this](const CommandContext&, Why&) {
        ctl_.next_tool();
        return true;
    });

    simple("zoom.in", [this](const CommandContext&, Why&) {
        ctl_.zoom_in();
        return true;
    });
    simple("zoom.out", [this](const CommandContext&, Why&) {
        ctl_.zoom_out();
        return true;
    });
    simple("zoom.vertical_in", [this](const CommandContext&, Why&) {
        ctl_.vertical_zoom(true);
        return true;
    });
    simple("zoom.vertical_out", [this](const CommandContext&, Why&) {
        ctl_.vertical_zoom(false);
        return true;
    });
    simple("zoom.fit_session",
           [this](const CommandContext&, Why& why) { return ctl_.zoom_fit_session(why); });
    simple("zoom.fit_selection",
           [this](const CommandContext&, Why& why) { return ctl_.zoom_fit_selection(why); });
    simple("zoom.toggle", [this](const CommandContext&, Why& why) { return ctl_.zoom_toggle(why); });

    simple("edit.undo", [this](const CommandContext&, Why& why) { return ctl_.undo(why); });
    simple("edit.redo", [this](const CommandContext&, Why& why) { return ctl_.redo(why); });
    simple("edit.clear", [this](const CommandContext&, Why& why) { return ctl_.clear(why); });
    simple("edit.separate_at_selection",
           [this](const CommandContext&, Why& why) { return ctl_.separate(why); });
    simple("edit.trim_start_to_insertion",
           [this](const CommandContext&, Why& why) { return ctl_.trim_start_to_insertion(why); });
    simple("edit.trim_end_to_insertion",
           [this](const CommandContext&, Why& why) { return ctl_.trim_end_to_insertion(why); });
    simple("nudge.forward", [this](const CommandContext&, Why& why) { return ctl_.nudge(1, why); });
    simple("nudge.back", [this](const CommandContext&, Why& why) { return ctl_.nudge(-1, why); });

    simple("track.select", [this](const CommandContext& ctx, Why& why) {
        if (ctx.target < 0 || ctx.target >= trackCount()) {
            why = "there is no track " + std::to_string(ctx.target + 1);
            return false;
        }
        ctl_.select_track(ctx.target);
        return true;
    });
    simple("track.select_next",
           [this](const CommandContext&, Why& why) { return ctl_.select_next_track(1, why); });
    simple("track.select_previous",
           [this](const CommandContext&, Why& why) { return ctl_.select_next_track(-1, why); });
    simple("track.toggle_mute", [this](const CommandContext& ctx, Why& why) {
        return ctl_.toggle_mute(ctx.has_target() ? ctx.target : ctl_.selection().track, why);
    });
    simple("track.toggle_solo", [this](const CommandContext& ctx, Why& why) {
        return ctl_.toggle_solo(ctx.has_target() ? ctx.target : ctl_.selection().track, why);
    });

    // Saving is the engine's once it has session commands (session_files.h);
    // until then the interface writes the file itself.
    simple("app.save", [this](const CommandContext&, Why& why) { return saveSession(why); });

    simple("window.toggle_mix_edit", [this](const CommandContext&, Why&) {
        setWindow(window_ == QStringLiteral("edit") ? QStringLiteral("mix") : QStringLiteral("edit"));
        return true;
    });
}

void SessionModel::attachItem(SceneItem* item) {
    if (std::find(items_.begin(), items_.end(), item) == items_.end()) items_.push_back(item);
}

void SessionModel::detachItem(SceneItem* item) {
    items_.erase(std::remove(items_.begin(), items_.end(), item), items_.end());
}

void SessionModel::sync() {
    const Snapshot& s = ctl_.snapshot();
    bool content = false;
    if (s.revision != seen_revision_) {
        seen_revision_ = s.revision;
        content = true;
        emit contentChanged();
        emit mixChanged();
        if (dirty() != seen_dirty_) {
            seen_dirty_ = dirty();
            emit sessionChanged();
        }
    }
    const int m = static_cast<int>(ctl_.mode()), t = static_cast<int>(ctl_.tool()),
              g = static_cast<int>(ctl_.grid());
    if (m != seen_mode_ || t != seen_tool_ || g != seen_grid_) {
        content = content || m != seen_mode_ || g != seen_grid_;  // grid lines follow them
        seen_mode_ = m;
        seen_tool_ = t;
        seen_grid_ = g;
        emit editStateChanged();
    }
    bool overlay = false;
    if (!seen_sel_valid_ || !same(seen_sel_, ctl_.selection())) {
        seen_sel_ = ctl_.selection();
        seen_sel_valid_ = true;
        content = overlay = true;
        emit selectionChanged();
        updateCounters(true);
    }
    if (!seen_view_valid_ || !same(seen_view_, ctl_.view())) {
        seen_view_ = ctl_.view();
        seen_view_valid_ = true;
        content = overlay = true;
        emit viewChanged();
    }
    if (!same(seen_drag_, ctl_.drag())) {
        seen_drag_ = ctl_.drag();
        content = true;
    }
    for (SceneItem* item : items_) {
        if (content) item->markContentDirty();
        if (overlay) item->markOverlayDirty();
    }
}

bool SessionModel::run(bool ok, const std::string& why) {
    sync();
    if (!ok && !why.empty()) emit refused(q(why));
    return ok;
}

void SessionModel::onTransport(const TransportSnapshot& t, bool alive) {
    const bool rolling = alive && !t.stale && t.state != TransportMode::Stopped;
    const bool recording = rolling && t.state == TransportMode::Recording;
    const auto playhead = static_cast<Frames>(t.playhead_frames);
    const bool changed = rolling != overlay_.rolling || recording != overlay_.recording ||
                         (rolling && playhead != overlay_.playhead);
    overlay_.rolling = rolling;
    overlay_.recording = recording;
    if (rolling) overlay_.playhead = playhead;
    if (!changed) return;
    if (rolling && ctl_.follow(playhead)) sync();
    for (SceneItem* item : items_) item->markOverlayDirty();
    updateCounters(++transport_ticks_ % kCounterEvery == 0 || !rolling);
}

void SessionModel::updateCounters(bool force) {
    if (!force) return;
    const Frames at = overlay_.rolling ? overlay_.playhead : ctl_.selection().insertion;
    const QString main = q(format_time(main_fmt_, at, ctl_.snapshot().settings));
    const QString sub = q(format_time(sub_fmt_, at, ctl_.snapshot().settings));
    if (main == main_counter_ && sub == sub_counter_) return;
    main_counter_ = main;
    sub_counter_ = sub;
    emit counterChanged();
}

// --- properties ---

QString SessionModel::sessionName() const {
    const std::string& n = ctl_.snapshot().settings.name;
    QString name = n.empty() ? QStringLiteral("Untitled session") : q(n);
    if (dirty()) name += QStringLiteral(" · edited");
    if (synthetic()) name += QStringLiteral(" · synthetic audio");
    return name;
}

bool SessionModel::synthetic() const {
    return synthetic_peaks_ && !ctl_.snapshot().sources->sources.empty();
}

int SessionModel::trackCount() const {
    return static_cast<int>(ctl_.snapshot().edit->tracks.size());
}

QStringList SessionModel::trackNames() const {
    QStringList out;
    for (const auto& t : ctl_.snapshot().edit->tracks) out << q(t->name);
    return out;
}

QString SessionModel::selectionStart() const {
    return q(format_time(main_fmt_, ctl_.selection().start, ctl_.snapshot().settings));
}

QString SessionModel::selectionEnd() const {
    return q(format_time(main_fmt_, ctl_.selection().end, ctl_.snapshot().settings));
}

QString SessionModel::selectionLength() const {
    const Selection& s = ctl_.selection();
    return q(format_min_sec(s.end - s.start, ctl_.snapshot().settings.sample_rate));
}

QString SessionModel::zoomText() const {
    const double px_per_s = ctl_.snapshot().settings.sample_rate / ctl_.view().fpp;
    return px_per_s >= 10 ? QString::number(px_per_s, 'f', 0) + QStringLiteral(" px/s")
                          : QString::number(1.0 / px_per_s, 'f', 1) + QStringLiteral(" s/px");
}

QList<bool> SessionModel::mutes() const {
    QList<bool> out;
    for (const auto& m : ctl_.snapshot().mix.tracks) out << m.mute;
    return out;
}

QList<bool> SessionModel::solos() const {
    QList<bool> out;
    for (const auto& m : ctl_.snapshot().mix.tracks) out << m.solo;
    return out;
}

QList<qreal> SessionModel::gains() const {
    QList<qreal> out;
    for (const auto& m : ctl_.snapshot().mix.tracks) out << m.gain_db;
    return out;
}

QList<qreal> SessionModel::pans() const {
    QList<qreal> out;
    for (const auto& m : ctl_.snapshot().mix.tracks) out << m.pan;
    return out;
}

bool SessionModel::anySolo() const {
    for (const auto& m : ctl_.snapshot().mix.tracks) {
        if (m.solo) return true;
    }
    return false;
}

bool SessionModel::loopOn() const {
    const auto& l = ctl_.snapshot().settings.transport.loop;
    return l[1] > l[0];
}

bool SessionModel::punchOn() const {
    const auto& p = ctl_.snapshot().settings.transport.punch;
    return p[1] > p[0];
}

QString SessionModel::tempoText() const {
    const auto& tempo = ctl_.snapshot().settings.tempo;
    if (tempo.empty()) return {};
    const auto& t = tempo.front();
    return QStringLiteral("%1 bpm  %2/%3")
        .arg(static_cast<double>(t.bpm), 0, 'f', 0)
        .arg(t.meter[0])
        .arg(t.meter[1]);
}

// --- Edit window ---

void SessionModel::setViewport(qreal width, qreal height) {
    ctl_.set_viewport(width, height);
    sync();
}

void SessionModel::press(qreal x, qreal y) {
    std::string why;
    run(ctl_.press(x, y, why), why);
}

void SessionModel::drag(qreal x, qreal y) {
    ctl_.move(x, y);
    sync();
}

void SessionModel::release(qreal x, qreal y) {
    std::string why;
    run(ctl_.release(x, y, why), why);
}

void SessionModel::wheel(qreal x, qreal dx, qreal dy, bool shift, bool ctrl) {
    if (ctrl) {
        ctl_.zoom_by(dy > 0 ? 1.0 / 1.25 : 1.25, x);
    } else if (shift || std::abs(dx) > std::abs(dy)) {
        ctl_.scroll_by(-(shift ? dy : dx), 0);
    } else {
        ctl_.scroll_by(0, -dy);
    }
    sync();
}

void SessionModel::rulerClick(qreal x) {
    ctl_.set_insertion(ctl_.view().frame_at(x));
    sync();
}

void SessionModel::selectTrack(int index) {
    ctl_.select_track(index);
    sync();
}

void SessionModel::cycleGrid() {
    ctl_.set_grid(next_grid_value(ctl_.grid()));
    sync();
}

void SessionModel::cycleMainCounter() {
    // Bars|Beats -> Min:Secs -> Timecode -> Samples, as the counter's
    // format menu runs; the sub counter shows Min:Secs, or Bars|Beats when
    // the main one already does.
    switch (main_fmt_) {
        case TimeFormat::BarsBeats: main_fmt_ = TimeFormat::MinSec; break;
        case TimeFormat::MinSec: main_fmt_ = TimeFormat::Timecode; break;
        case TimeFormat::Timecode: main_fmt_ = TimeFormat::Samples; break;
        case TimeFormat::Samples: main_fmt_ = TimeFormat::BarsBeats; break;
    }
    sub_fmt_ = main_fmt_ == TimeFormat::MinSec ? TimeFormat::BarsBeats : TimeFormat::MinSec;
    main_counter_.clear();
    updateCounters(true);
    emit selectionChanged();
}

void SessionModel::setWindow(const QString& w) {
    if (w == window_ || (w != QStringLiteral("edit") && w != QStringLiteral("mix"))) return;
    window_ = w;
    emit windowChanged();
}

// --- File ---

bool SessionModel::saveSession(std::string& why) {
    QString file = file_;
    if (file.isEmpty()) {
        const std::string& n = ctl_.snapshot().settings.name;
        const QString wanted = n.empty() ? QStringLiteral("Untitled") : q(n);
        const std::string bad = session_name_problem(wanted);
        if (!bad.empty()) {
            why = "cannot save as \"" + n + "\": " + bad;
            return false;
        }
        const QString name = unused_session_name(sessions_dir_, wanted);
        // The session is called what its folder is called, so the name the
        // window shows is the one the Open list shows.
        if (name.toStdString() != n) {
            model::SessionSettings settings = ctl_.snapshot().settings;
            settings.name = name.toStdString();
            if (!doc_.submit(model::SetSettings{settings}, why)) return false;
        }
        file = session_file_path(sessions_dir_, name);
    }

    // Canonical bytes (session_file.h): the same content is the same bytes,
    // so a save that would write what the file already holds writes nothing
    // and keeps the generation, as session-format.md asks.
    model::WriteInfo info;
    info.written_by = "sonora-ui";
    info.save_generation = save_generation_;
    std::string bytes, error;
    if (!model::serialize(ctl_.snapshot(), info, bytes, error)) {
        why = "the session could not be saved: " + error;
        sync();
        return false;
    }
    const bool nothing_new = file == file_ && bytes == saved_bytes_;
    if (!nothing_new) {
        info.save_generation = save_generation_ + 1;
        if (!model::serialize(ctl_.snapshot(), info, bytes, error)) {
            why = "the session could not be saved: " + error;
            sync();
            return false;
        }
        if (!write_session_file(file, bytes, why)) {
            sync();
            return false;
        }
        save_generation_ = info.save_generation;
        saved_bytes_ = std::move(bytes);
    }
    file_ = file;
    saved_revision_ = doc_.revision();
    seen_dirty_ = false;
    sync();
    emit sessionChanged();
    emit notice(nothing_new ? QStringLiteral("Nothing to save: ") + file_ +
                                  QStringLiteral(" is current")
                            : QStringLiteral("Saved ") + file_);
    return true;
}

bool SessionModel::openSessionFile(const QString& file, bool discard, std::string& why) {
    if (overlay_.recording) {
        why = "a take is open; stop recording before opening another session";
        return false;
    }
    if (dirty() && !discard) {
        why = "this session has unsaved edits; save it first, or open with \"Discard edits\"";
        return false;
    }
    std::string bytes;
    model::Loaded loaded;
    if (!read_session_file(file, bytes, loaded, why)) return false;
    const uint32_t rate = ctl_.snapshot().settings.sample_rate;
    if (loaded.snapshot.settings.sample_rate != rate) {
        why = "the session is at " + std::to_string(loaded.snapshot.settings.sample_rate) +
              " Hz and this system runs at " + std::to_string(rate) +
              " Hz; it cannot play here until sample-rate conversion exists";
        return false;
    }
    doc_.reset(std::move(loaded.snapshot));
    ctl_.document_replaced();
    file_ = file;
    saved_revision_ = doc_.revision();
    save_generation_ = loaded.save_generation;
    saved_bytes_ = std::move(bytes);
    seen_dirty_ = false;
    // Everything the windows show came from the old session.
    seen_revision_ = UINT64_MAX;
    seen_sel_valid_ = seen_view_valid_ = false;
    updateCounters(true);
    sync();
    emit sessionChanged();
    emit notice(QStringLiteral("Opened ") + file_);
    return true;
}

QVariantList SessionModel::sessionList() const {
    QVariantList out;
    for (const SessionEntry& e : list_sessions(sessions_dir_)) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), e.name);
        m.insert(QStringLiteral("file"), e.file);
        m.insert(QStringLiteral("current"), e.file == file_);
        out << m;
    }
    return out;
}

bool SessionModel::openSession(const QString& file, bool discard) {
    std::string why;
    return run(openSessionFile(file, discard, why), why);
}

// --- Mix window ---

qreal SessionModel::faderPosition(qreal db) const {
    return fader_position_from_db(static_cast<float>(db));
}

QString SessionModel::gainText(qreal db) const { return q(sonora::ui::gain_text(static_cast<float>(db))); }

QString SessionModel::panText(qreal pan) const { return q(sonora::ui::pan_text(static_cast<float>(pan))); }

void SessionModel::faderDrag(int track, qreal dy, qreal travel, bool fine) {
    const Snapshot& s = ctl_.snapshot();
    const float now = track < 0 ? s.mix.bus.gain_db
                      : track < static_cast<int>(s.mix.tracks.size())
                          ? s.mix.tracks[static_cast<std::size_t>(track)].gain_db
                          : 0.0f;
    setGain(track, fader_drag(now, static_cast<float>(dy), static_cast<float>(travel), fine));
}

void SessionModel::resetGain(int track) { setGain(track, 0.0); }

void SessionModel::setGain(int track, qreal db) {
    std::string why;
    run(track < 0 ? ctl_.set_bus_gain(static_cast<float>(db), why)
                  : ctl_.set_gain(track, static_cast<float>(db), why),
        why);
}

void SessionModel::panDrag(int track, qreal dx, qreal width) {
    const auto& ts = ctl_.snapshot().mix.tracks;
    if (track < 0 || track >= static_cast<int>(ts.size()) || width <= 0) return;
    const float now = ts[static_cast<std::size_t>(track)].pan;
    std::string why;
    run(ctl_.set_pan(track, std::clamp(now + static_cast<float>(2.0 * dx / width), -1.0f, 1.0f), why),
        why);
}

void SessionModel::resetPan(int track) {
    std::string why;
    run(ctl_.set_pan(track, 0.0f, why), why);
}

// --- scene ---

void SessionModel::buildTimeline(Scene& out) const {
    build_timeline_scene(ctl_.snapshot(), ctl_.view(), ctl_.selection(), ctl_.drag(),
                         ctl_.grid_lines(), ctl_.grid(), peaks_, out);
}

void SessionModel::buildOverlay(std::vector<SceneRect>& out) const {
    build_timeline_overlay(ctl_.view(), ctl_.selection(), overlay_, out);
}

void SessionModel::buildRuler(TimeFormat fmt, double height, Scene& out) const {
    build_ruler_scene(fmt, ctl_.snapshot().settings, ctl_.view(), height, out);
}

}  // namespace sonora::ui
