// SPDX-License-Identifier: GPL-3.0-or-later
//
// Groups in the Mix window (ADR-0041): what the strips and the group buttons
// read, the group operations, and the other members of a group following a
// fader ride. See mix_model.h.
#include <QList>
#include <QStringList>
#include <algorithm>
#include <utility>

#include "fader_taper.h"
#include "mix_model.h"
#include "session_model.h"
#include "sonora/model/groups.h"

namespace sonora::ui {

QList<int> MixModel::trackGroups() const {
    QList<int> out;
    const EditController& c = session_.controller();
    const int n = session_.trackCount();
    for (int i = 0; i < n; ++i) out << c.group_index(i);
    return out;
}

QStringList MixModel::groupNames() const {
    QStringList out;
    for (const model::Group& g : session_.controller().snapshot().edit->groups) {
        out << QString::fromStdString(g.name);
    }
    return out;
}

QList<bool> MixModel::groupSuspended() const {
    QList<bool> out;
    for (const model::Group& g : session_.controller().snapshot().edit->groups) {
        out << !g.active;
    }
    return out;
}

QStringList MixModel::groupSummaries() const {
    QStringList out;
    for (const model::Group& g : session_.controller().snapshot().edit->groups) {
        QStringList linked;
        if (g.attrs & model::kGroupVolume) {
            linked << (g.relative ? QStringLiteral("Volume (Relative)")
                                  : QStringLiteral("Volume (Absolute)"));
        }
        if (g.attrs & model::kGroupMute) linked << QStringLiteral("Mute");
        if (g.attrs & model::kGroupSolo) linked << QStringLiteral("Solo");
        if (g.attrs & model::kGroupArm) linked << QStringLiteral("Arm");
        if (g.attrs & model::kGroupSelection) linked << QStringLiteral("Selection");
        if (g.attrs & model::kGroupEdit) linked << QStringLiteral("Edit");
        out << linked.join(QStringLiteral(", ")) + (g.active ? QString() : QStringLiteral(" (suspended)"));
    }
    return out;
}

bool MixModel::groupSelected(const QString& name) {
    const Selection& s = session_.controller().selection();
    std::string why;
    return session_.reportEdit(
        session_.controller().group_tracks(
            s.track, s.tracks, name.toStdString(),
            model::kGroupVolume | model::kGroupMute | model::kGroupSolo | model::kGroupArm, why),
        why);
}

bool MixModel::dissolveGroup(int track) {
    std::string why;
    return session_.reportEdit(session_.controller().dissolve_group(track, why), why);
}

bool MixModel::suspendGroup(int track) {
    std::string why;
    return session_.reportEdit(session_.controller().toggle_group_suspend(track, why), why);
}

bool MixModel::setGroupRelative(int track, bool relative) {
    std::string why;
    return session_.reportEdit(session_.controller().set_group_relative(track, relative, why),
                               why);
}

bool MixModel::toggleGroupAttribute(int track, const QString& attribute) {
    static const std::pair<const char*, std::uint8_t> kinds[] = {{"volume", model::kGroupVolume},
                                                                  {"mute", model::kGroupMute},
                                                                  {"solo", model::kGroupSolo},
                                                                  {"arm", model::kGroupArm}};
    for (const auto& k : kinds) {
        if (attribute != QLatin1String(k.first)) continue;
        std::string why;
        return session_.reportEdit(session_.controller().toggle_group_attribute(track, k.second, why),
                                   why);
    }
    return session_.reportEdit(false, "a group links volume, mute, solo or arm, not " +
                                          attribute.toStdString());
}

void MixModel::beginPeers(int track) {
    ride_peers_.clear();
    if (track < 0 || track >= session_.trackCount()) return;
    const model::EditState& edit = *session_.controller().snapshot().edit;
    const model::Group* g = model::linking_group(edit, session_.trackId(track), model::kGroupVolume);
    if (g == nullptr) return;
    ride_group_ = *g;
    for (const model::Id id : g->members) {
        const int index = session_.trackIndex(id);
        if (index < 0 || index == track) continue;
        RidePeer p{index, id, session_.stripGain(index), session_.stripGain(index), false};
        // A peer the engine does not have still follows the saved SetMix.
        if (live(index)) {
            sender_(static_cast<uint32_t>(index), api::ParamId::Gain, fader_linear_gain(p.from_db),
                    api::Gesture::Touch);
            p.touched = true;
        }
        ride_peers_.push_back(p);
    }
}

void MixModel::movePeers(float db) {
    for (RidePeer& p : ride_peers_) {
        if (!p.touched || session_.trackIndex(p.id) != p.index) continue;
        const float g = std::clamp(
            model::group_follow_gain(ride_group_, p.from_db, ride_from_db_, db),
            std::min(kFaderSilenceDb, p.from_db), std::max(kFaderMaxDb, p.from_db));
        p.last_db = g;
        sender_(static_cast<uint32_t>(p.index), api::ParamId::Gain, fader_linear_gain(g),
                api::Gesture::Move);
        ++live_steps_;
    }
}

void MixModel::releasePeers(bool to_document) {
    for (RidePeer& p : ride_peers_) {
        if (!p.touched && !to_document) continue;
        if (!live(p.index)) continue;
        // A dropped ride puts the engine back to the document's value for
        // whatever track now sits at the index it touched.
        const float db = to_document && p.index < session_.trackCount()
                             ? session_.stripGain(p.index)
                             : p.last_db;
        sender_(static_cast<uint32_t>(p.index), api::ParamId::Gain, fader_linear_gain(db),
                api::Gesture::Release);
        p.touched = false;
    }
}

}  // namespace sonora::ui
