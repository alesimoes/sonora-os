// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_model.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

#include "fader_taper.h"
#include "scene_item.h"
#include "edit_clipboard.h"
#include "session_files.h"
#include "sonora/api/consolidate_argument.h"
#include "sonora/api/messages.h"
#include "sonora/api/render.h"
#include "sonora/command.h"
#include "sonora/model/render_in_place.h"
#include "sonora/model/session_file.h"
#include "sonora/model/session_template.h"
#include "transport_model.h"
#include "session_model_support.h"

namespace sonora::ui {

using namespace model;

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

QList<int> SessionModel::trackColours() const {
    QList<int> out;
    for (const auto& t : ctl_.snapshot().edit->tracks) out << t->colour;
    return out;
}

QVariantList SessionModel::trackRows() const {
    const Snapshot& s = ctl_.snapshot();
    const ViewState& v = ctl_.view();
    QVariantList rows;
    for (std::size_t i = 0; i < s.edit->tracks.size(); ++i) {
        const TrackEdit& t = *s.edit->tracks[i];
        QVariantMap row;
        row[QStringLiteral("top")] = v.tops.size() > i ? v.tops[i] : static_cast<qreal>(i) * v.track_height;
        row[QStringLiteral("height")] = v.row_h(static_cast<int>(i));
        row[QStringLiteral("hidden")] = v.row_hidden(static_cast<int>(i));
        row[QStringLiteral("shown")] = v.autolanes_open(static_cast<int>(i));
        row[QStringLiteral("canShow")] = t.kind != TrackKind::Midi;
        // Take lanes (ADR-0031 §1): the other playlists, shown under the
        // track (and under its automation lanes) when lanesShown.
        QStringList lane_names;
        for (const std::string& n : ctl_.take_lane_names(static_cast<int>(i))) lane_names << q(n);
        row[QStringLiteral("takes")] = static_cast<int>(lane_names.size());
        row[QStringLiteral("laneNames")] = lane_names;
        row[QStringLiteral("lanesShown")] = ctl_.comp_lanes_shown(static_cast<int>(i));
        row[QStringLiteral("lanesTop")] =
            v.autolanes_open(static_cast<int>(i)) ? kLaneCount * v.autolane_height : 0.0;
        // What each lane is, said in the words of what the engine does with
        // it: a lane the mode leaves unplayed is not "automation" to the ear.
        const TrackMix* mix = s.mix.find(t.id);
        const char* mode = "Read";
        if (mix != nullptr) {
            switch (mix->automation) {
                case AutomationMode::Off: mode = "Off"; break;
                case AutomationMode::Read: mode = "Read"; break;
                case AutomationMode::Touch: mode = "Touch"; break;
                case AutomationMode::Latch: mode = "Latch"; break;
                case AutomationMode::Write: mode = "Write"; break;
                case AutomationMode::Trim: mode = "Trim"; break;
                case AutomationMode::WriteTrim: mode = "Write/Trim"; break;
            }
        }
        QStringList captions;
        for (int l = 0; l < kLaneCount; ++l) {
            const AutomationLane* lane = t.lane(lane_param(l));
            // A trim with no lane has no static value: it plays 0 dB.
            QString what = lane != nullptr ? QStringLiteral("%1 pts").arg(lane->points.size())
                           : lane_param(l) == AutomationParam::VolumeTrim ? QStringLiteral("0 dB")
                                                                          : QStringLiteral("static");
            captions << what + QStringLiteral("  ") + QChar(0x00B7) + QStringLiteral("  ") + QString::fromLatin1(mode);
        }
        row[QStringLiteral("captions")] = captions;
        rows.push_back(row);
    }
    return rows;
}

QVariantMap SessionModel::laneSelection() const {
    QVariantMap m;
    const auto sel = ctl_.lane_selection();
    int index = -1;
    if (sel.valid()) {
        const auto ids = ctl_.take_lane_ids(sel.track);
        const auto it = std::find(ids.begin(), ids.end(), sel.lane);
        if (it != ids.end()) index = static_cast<int>(it - ids.begin());
    }
    m[QStringLiteral("track")] = sel.valid() ? sel.track : -1;
    m[QStringLiteral("lane")] = index;
    m[QStringLiteral("ready")] = index >= 0;
    return m;
}

QStringList SessionModel::playlistNames() const {
    QStringList out;
    const int n = static_cast<int>(ctl_.snapshot().edit->tracks.size());
    for (int i = 0; i < n; ++i) out << q(ctl_.active_playlist_name(i));
    return out;
}

QStringList SessionModel::trackPlaylists(int track) const {
    QStringList out;
    for (const std::string& name : ctl_.playlist_names(track)) out << q(name);
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

QList<bool> SessionModel::mutes() const {
    return per_strip<bool>(ctl_.snapshot(), [](const model::TrackMix& m) { return m.mute; });
}

QList<bool> SessionModel::solos() const {
    return per_strip<bool>(ctl_.snapshot(), [](const model::TrackMix& m) { return m.solo; });
}

QList<bool> SessionModel::soloSafes() const {
    return per_strip<bool>(ctl_.snapshot(), [](const model::TrackMix& m) { return m.solo_safe; });
}

QStringList SessionModel::autolaneNames() const {
    QStringList out;
    for (int l = 0; l < kLaneCount; ++l) out << QString::fromLatin1(lane_name(lane_param(l)));
    return out;
}

QStringList SessionModel::automationModes() const {
    // Indexed by AutomationMode: one name for each.
    static const char* const names[] = {"Off",   "Read", "Touch",     "Latch",
                                        "Write", "Trim", "Write/Trim"};
    static_assert(sizeof names / sizeof names[0] ==
                  static_cast<std::size_t>(model::AutomationMode::WriteTrim) + 1);
    QStringList out;
    for (const QString& n : per_strip<QString>(ctl_.snapshot(), [](const model::TrackMix& m) {
             return QString::fromLatin1(names[static_cast<int>(m.automation)]);
         })) {
        out << n;
    }
    return out;
}

QList<bool> SessionModel::inputMonitors() const {
    return per_strip<bool>(ctl_.snapshot(), [](const model::TrackMix& m) {
        return m.monitor == model::MonitorMode::Input;
    });
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

// The mode, not the range: Pro Tools lights Loop Playback with no
// selection, and a range kept with the mode off does not loop.
bool SessionModel::loopOn() const { return ctl_.snapshot().settings.transport.loop_playback; }

// Pro Tools' Loop Record, apart from Loop Playback: what makes Record a
// loop take.
bool SessionModel::loopRecordOn() const {
    return ctl_.snapshot().settings.transport.loop_record;
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

}  // namespace sonora::ui
