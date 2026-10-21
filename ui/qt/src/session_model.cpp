// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_model.h"

#include <QTimer>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <memory>

#include "fader_taper.h"
#include "scene_item.h"
#include "edit_clipboard.h"
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
      writer_(write_session_file),
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
    simple("edit.cut", [this](const CommandContext&, Why& why) { return ctl_.cut(why); });
    simple("edit.copy", [this](const CommandContext&, Why& why) { return ctl_.copy(why); });
    simple("edit.paste", [this](const CommandContext&, Why& why) { return ctl_.paste(why); });
    simple("edit.duplicate",
           [this](const CommandContext&, Why& why) { return ctl_.duplicate(why); });
    // Pro Tools' Repeat asks for the number of repeats first. The key sends
    // no count, so it opens the window's dialog, which sends the count back
    // as the argument; there is nothing to ask about with nothing to repeat.
    simple("edit.repeat", [this](const CommandContext& ctx, Why& why) {
        if (ctx.argument.empty()) {
            std::string name;
            Frames length = 0;
            if (!ctl_.can_repeat(why, &name, &length)) return false;
            emit repeatRequested(ctl_.repeat_count(),
                                 q(format_min_sec(length, ctl_.snapshot().settings.sample_rate)) +
                                     QStringLiteral(" of ") + q(name));
            return true;
        }
        int count = 0;
        if (!parse_repeat_count(ctx.argument, count)) {
            why = "\"" + ctx.argument +
                  "\" is not a number of repeats; type a whole number, 1 or more. Nothing was "
                  "repeated.";
            return false;
        }
        return ctl_.repeat(count, why);
    });
    simple("edit.separate_at_selection",
           [this](const CommandContext&, Why& why) { return ctl_.separate(why); });
    simple("edit.trim_start_to_insertion",
           [this](const CommandContext&, Why& why) { return ctl_.trim_start_to_insertion(why); });
    simple("edit.trim_end_to_insertion",
           [this](const CommandContext&, Why& why) { return ctl_.trim_end_to_insertion(why); });
    simple("edit.fade_to_start",
           [this](const CommandContext&, Why& why) { return ctl_.fade_to_start(why); });
    simple("edit.fade_to_end",
           [this](const CommandContext&, Why& why) { return ctl_.fade_to_end(why); });
    simple("edit.fade_selection",
           [this](const CommandContext&, Why& why) { return ctl_.fade_selection(why); });
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
    const int fades = static_cast<int>(ctl_.fade_shape()) * 16 +
                      static_cast<int>(ctl_.crossfade_shape());
    const uint64_t clip = ctl_.clipboard_serial();
    if (m != seen_mode_ || t != seen_tool_ || g != seen_grid_ || fades != seen_fades_ ||
        clip != seen_clip_) {
        seen_clip_ = clip;
        content = content || m != seen_mode_ || g != seen_grid_;  // grid lines follow them
        seen_mode_ = m;
        seen_tool_ = t;
        seen_grid_ = g;
        seen_fades_ = fades;
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

QString SessionModel::undoText() const {
    if (!doc_.can_undo()) return QStringLiteral("Can't Undo");
    const std::string what = step_text(doc_.undo_label());
    return what.empty() ? QStringLiteral("Undo") : QStringLiteral("Undo ") + q(what);
}

QString SessionModel::redoText() const {
    if (!doc_.can_redo()) return QStringLiteral("Can't Redo");
    const std::string what = step_text(doc_.redo_label());
    return what.empty() ? QStringLiteral("Redo") : QStringLiteral("Redo ") + q(what);
}

bool SessionModel::run(bool ok, const std::string& why) {
    sync();
    if (!ok && !why.empty()) emit refused(q(why));
    return ok;
}

void SessionModel::documentUpdated() {
    const model::Snapshot& s = ctl_.snapshot();
    if (ctl_.selection().track >= static_cast<int>(s.edit->tracks.size())) {
        ctl_.document_replaced();
    } else if (ctl_.selection().region != kNoId &&
               find_region(s, ctl_.selection().region) == nullptr) {
        ctl_.clear_selection();
    }
    // Another interface's edit can arrive at the revision an edit of ours
    // was shown at, so the revision alone does not say the content changed.
    seen_revision_ = UINT64_MAX;
    sync();
}

void SessionModel::onTransport(const TransportSnapshot& t, bool alive) {
    // A stale read is a seqlock miss, not a stop (ADR-0007 §3): keep what is
    // shown, as TransportModel does, or the bar counter drops to the
    // insertion point while the Transport window keeps the playhead.
    if (alive && t.stale) return;
    const bool rolling = alive && t.state != TransportMode::Stopped;
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

// The Mix window's strips are the Edit window's tracks, in display order.
// MixState::tracks is keyed by id, in no display order, and keeps the
// entries of undone AddTracks: indexing it by strip showed one track's
// fader on another's strip once tracks were reordered or one was undone.
namespace {
template <typename T, typename F>
QList<T> per_strip(const model::Snapshot& s, F field) {
    QList<T> out;
    for (const auto& t : s.edit->tracks) {
        const model::TrackMix* m = s.mix.find(t->id);
        out << (m != nullptr ? field(*m) : field(model::TrackMix{}));
    }
    return out;
}

const model::TrackMix* strip_mix(const model::Snapshot& s, int track) {
    if (track < 0 || track >= static_cast<int>(s.edit->tracks.size())) return nullptr;
    return s.mix.find(s.edit->tracks[static_cast<std::size_t>(track)]->id);
}
}  // namespace

QList<bool> SessionModel::mutes() const {
    return per_strip<bool>(ctl_.snapshot(), [](const model::TrackMix& m) { return m.mute; });
}

QList<bool> SessionModel::solos() const {
    return per_strip<bool>(ctl_.snapshot(), [](const model::TrackMix& m) { return m.solo; });
}

QList<bool> SessionModel::soloSafes() const {
    return per_strip<bool>(ctl_.snapshot(), [](const model::TrackMix& m) { return m.solo_safe; });
}

QList<bool> SessionModel::implicitMutes() const {
    const bool any = anySolo();
    return per_strip<bool>(ctl_.snapshot(), [any](const model::TrackMix& m) {
        return any && !m.solo && !m.solo_safe;
    });
}

QList<qreal> SessionModel::gains() const {
    return per_strip<qreal>(ctl_.snapshot(),
                            [](const model::TrackMix& m) { return qreal(m.gain_db); });
}

QList<qreal> SessionModel::pans() const {
    return per_strip<qreal>(ctl_.snapshot(), [](const model::TrackMix& m) { return qreal(m.pan); });
}

bool SessionModel::anySolo() const {
    // An undone track's solo silences nothing that is still there.
    for (const bool solo : solos()) {
        if (solo) return true;
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

void SessionModel::cycleFadeShape() {
    ctl_.set_fade_shape(next_fade_shape(ctl_.fade_shape()));
    sync();
}

void SessionModel::cycleCrossfadeShape() {
    ctl_.set_crossfade_shape(next_fade_shape(ctl_.crossfade_shape()));
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

namespace {

bool has_inserts(const Snapshot& s) {
    for (const auto& t : s.edit->tracks) {
        if (!t->inserts.empty()) return true;
    }
    return false;
}

}  // namespace

bool SessionModel::saveSession(std::string& why) {
    if (save_waiting_) {
        if (save_wait_.elapsed() < flush_patience_ms_) {
            why = "a save is already waiting for the plugins' state from the engine";
            return false;
        }
        save_waiting_ = false;
        ++save_ticket_;   // its answer, if it ever comes, is ignored
        return writeSession(why, "the engine never sent the plugins' state, so plugin "
                                 "settings are as last recorded");
    }
    // No insert, no plugin state: nothing to wait for.
    if (!has_inserts(ctl_.snapshot())) return writeSession(why, {});

    save_waiting_ = true;
    save_wait_.start();
    const uint64_t ticket = ++save_ticket_;
    // The flush may answer inside this call (no engine in between) or from
    // a later tick; the first reports through `why`, the second on its own.
    struct Inline {
        bool inside = true;
        bool answered = false;
        bool saved = false;
        std::string why;
    };
    auto now = std::make_shared<Inline>();
    doc_.flush_plugin_state(kNoId, [this, ticket, now](bool ok, const std::string& flush_why) {
        if (ticket != save_ticket_) return;
        save_waiting_ = false;
        std::string why;
        const bool saved = writeSession(
            why, ok ? std::string() : "these plugins' settings are as last recorded: " + flush_why);
        if (now->inside) {
            now->answered = true;
            now->saved = saved;
            now->why = why;
        } else if (!saved && !why.empty()) {
            emit refused(q(why));
        }
    });
    now->inside = false;
    if (now->answered) {
        why = now->why;
        return now->saved;
    }
    sync();
    emit notice(QStringLiteral("Saving: collecting the plugins' state from the engine"));
    // Never left waiting in silence: once the patience is spent the
    // operator is told what a second Save will do.
    QTimer::singleShot(static_cast<int>(flush_patience_ms_), this, [this, ticket]() {
        if (!save_waiting_ || ticket != save_ticket_) return;
        emit refused(QStringLiteral(
            "Not saved yet: the engine has not sent the plugins' state. Save again to write the "
            "session without it; plugin settings would be as last recorded."));
    });
    return true;
}

bool SessionModel::writeSession(std::string& why, std::string caution) {
    QString file = file_;
    // What is written: the session as it is, or, the first time, renamed to
    // its folder's name. The rename reaches the document only once the file
    // is written, so a failed save leaves the name and the disk as they were
    // and a retry is not pushed to "<Name> 2".
    Snapshot to_write = ctl_.snapshot();
    bool rename = false;
    if (file.isEmpty()) {
        const std::string& n = to_write.settings.name;
        const QString wanted = n.empty() ? QStringLiteral("Untitled") : q(n);
        std::string bad = session_name_problem(wanted);
        if (!bad.empty()) {
            why = "cannot save as \"" + n + "\": " + bad;
            return false;
        }
        const QString name = unused_session_name(sessions_dir_, wanted);
        bad = session_name_problem(name);
        if (!bad.empty()) {
            why = "cannot save as \"" + name.toStdString() + "\", the first free name: " + bad;
            return false;
        }
        // The session is called what its folder is called, so the name the
        // window shows is the one the Open list shows.
        if (name.toStdString() != n) {
            to_write.settings.name = name.toStdString();
            rename = true;
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
    if (!model::serialize(to_write, info, bytes, error, &sequences_.digests())) {
        why = "the session could not be saved: " + error;
        sync();
        return false;
    }
    const bool nothing_new = file == file_ && bytes == saved_bytes_;
    if (!nothing_new) {
        info.save_generation = save_generation_ + 1;
        if (!model::serialize(to_write, info, bytes, error, &sequences_.digests())) {
            why = "the session could not be saved: " + error;
            sync();
            return false;
        }
        // The sequence files first (ADR-0015 §3): the session file must never
        // name one that is not durable. A failure here leaves the old session
        // file as it was.
        if (!sequences_.write(file, to_write, why)) {
            why = "the session could not be saved: " + why;
            sync();
            return false;
        }
        std::string disk_caution;
        if (!writer_(file, bytes, why, disk_caution)) {
            sync();
            return false;
        }
        if (!disk_caution.empty()) {
            caution = caution.empty() ? disk_caution : caution + "; " + disk_caution;
        }
        sequences_.saved(file, to_write);
        // O4, after a durable save only: with the folder unsynced the old
        // session file can come back after a power cut, and its sequence
        // files must still be there. Held back too while a take is open
        // (§6): a deletion's journal commit is one more thing in front of
        // the take's own syncs.
        std::string clean_caution;
        if (disk_caution.empty() && !overlay_.recording) sequences_.clean(clean_caution);
        if (!clean_caution.empty()) {
            caution = caution.empty() ? clean_caution : caution + "; " + clean_caution;
        }
        save_generation_ = info.save_generation;
        saved_bytes_ = std::move(bytes);
    }
    file_ = file;
    // The file is on disk under the new name; the document follows it. A
    // refusal here would be a model bug, and the file already says the name.
    std::string rename_why;
    if (rename && !doc_.submit(model::SetSettings{to_write.settings}, rename_why)) {
        caution = caution.empty() ? "the window keeps the old name: " + rename_why
                                  : caution + "; the window keeps the old name: " + rename_why;
    }
    saved_revision_ = doc_.revision();
    seen_dirty_ = false;
    sync();
    emit sessionChanged();
    QString said = nothing_new ? QStringLiteral("Nothing to save: ") + file_ +
                                     QStringLiteral(" is current")
                               : QStringLiteral("Saved ") + file_;
    if (!caution.empty()) said += QStringLiteral(", but ") + q(caution);
    emit notice(said);
    return true;
}

bool SessionModel::openSessionFile(const QString& file, bool discard, std::string& why) {
    if (overlay_.recording) {
        why = "a take is open; stop recording before opening another session";
        return false;
    }
    if (save_waiting_) {
        why = "a save is waiting for the plugins' state; open the session once it is saved";
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
    const std::shared_ptr<const model::SourcePool> pool = loaded.snapshot.sources;
    if (!doc_.reset(std::move(loaded.snapshot), why)) return false;
    ctl_.document_replaced();
    file_ = file;
    sequences_.open(file, loaded.verified_sequences);
    // A repaired session is not the file on disk: it reads as modified, so
    // Save writes it and another Open asks first.
    saved_revision_ = loaded.repairs.empty() ? doc_.revision() : UINT64_MAX;
    save_generation_ = loaded.save_generation;
    saved_bytes_ = std::move(bytes);
    seen_dirty_ = false;
    // Everything the windows show came from the old session.
    seen_revision_ = UINT64_MAX;
    seen_sel_valid_ = seen_view_valid_ = false;
    updateCounters(true);
    sync();
    emit sessionChanged();
    QVariantList repairs;
    for (const model::Repair& r : loaded.repairs) {
        QVariantMap m;
        m.insert(QStringLiteral("path"), q(r.path));
        m.insert(QStringLiteral("was"), q(r.was));
        m.insert(QStringLiteral("now"), q(r.now));
        m.insert(QStringLiteral("why"), q(r.why));
        repairs << m;
    }
    // O2: each missing sequence file shown as a repair is, held until
    // dismissed. Its regions opened silent with their windows; nothing is
    // dropped, and a save names the file again.
    for (const model::MissingSequence& ms : loaded.missing_sequences) {
        std::string where = ms.regions.front();
        if (ms.regions.size() > 1) where += " and " + std::to_string(ms.regions.size() - 1) + " more";
        std::string why_missing = ms.why;
        const model::MidiSource* take =
            ms.sources.empty() ? nullptr : pool->find_midi(ms.sources.front());
        if (take != nullptr) why_missing += "; its take " + take->path + " is kept";
        QVariantMap m;
        m.insert(QStringLiteral("path"), q(where));
        m.insert(QStringLiteral("was"), q(ms.ref.file));
        m.insert(QStringLiteral("now"), QStringLiteral("MIDI data missing: silent, not editable"));
        m.insert(QStringLiteral("why"), q(why_missing));
        repairs << m;
    }
    setRepairs(std::move(repairs));
    QString said = QStringLiteral("Opened ") + file_;
    if (!loaded.repairs.empty()) {
        said += QStringLiteral(", with %1 value%2 repaired to open (listed below); save to keep them")
                    .arg(loaded.repairs.size())
                    .arg(loaded.repairs.size() == 1 ? QString() : QStringLiteral("s"));
    }
    if (!loaded.missing_sequences.empty()) {
        said += QStringLiteral(", with %1 MIDI sequence file%2 missing (listed below)")
                    .arg(loaded.missing_sequences.size())
                    .arg(loaded.missing_sequences.size() == 1 ? QString() : QStringLiteral("s"));
    }
    emit notice(said);
    return true;
}

void SessionModel::setRepairs(QVariantList r) {
    if (r.isEmpty() && repairs_.isEmpty()) return;
    repairs_ = std::move(r);
    emit repairsChanged();
}

void SessionModel::dismissRepair(int index) {
    if (index < 0 || index >= repairs_.size()) return;
    repairs_.removeAt(index);
    emit repairsChanged();
}

void SessionModel::openWasLost() {
    const QString lost = file_;
    file_.clear();
    saved_bytes_.clear();
    save_generation_ = 0;
    // Nothing on disk holds what is shown, so it reads as unsaved.
    saved_revision_ = UINT64_MAX;
    // What is shown is the engine's document, not the repaired file.
    setRepairs({});
    seen_revision_ = UINT64_MAX;
    sync();
    emit sessionChanged();
    if (!lost.isEmpty()) {
        emit notice(lost + QStringLiteral(" was not opened: the engine was unreachable. "
                                          "Open it again."));
    }
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
    const float now = stripGain(track);
    setGain(track, fader_drag(now, static_cast<float>(dy), static_cast<float>(travel), fine));
}

float SessionModel::stripGain(int track) const {
    const model::Snapshot& s = ctl_.snapshot();
    if (track < 0) return s.mix.bus.gain_db;
    const model::TrackMix* m = strip_mix(s, track);
    return m != nullptr ? m->gain_db : 0.0f;
}

model::Id SessionModel::trackId(int track) const {
    const auto& tracks = ctl_.snapshot().edit->tracks;
    if (track < 0 || track >= static_cast<int>(tracks.size())) return kNoId;
    return tracks[static_cast<std::size_t>(track)]->id;
}

int SessionModel::trackIndex(model::Id id) const {
    const auto& tracks = ctl_.snapshot().edit->tracks;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (tracks[i]->id == id) return static_cast<int>(i);
    }
    return -1;
}

float SessionModel::stripPan(int track) const {
    const model::TrackMix* m = strip_mix(ctl_.snapshot(), track);
    return m != nullptr ? m->pan : 0.0f;
}

void SessionModel::resetGain(int track) { setGain(track, 0.0); }

void SessionModel::setGain(int track, qreal db) {
    std::string why;
    run(track < 0 ? ctl_.set_bus_gain(static_cast<float>(db), why)
                  : ctl_.set_gain(track, static_cast<float>(db), why),
        why);
}

void SessionModel::panDrag(int track, qreal dx, qreal width) {
    const model::TrackMix* m = strip_mix(ctl_.snapshot(), track);
    if (m == nullptr || width <= 0) return;
    const float now = m->pan;
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
