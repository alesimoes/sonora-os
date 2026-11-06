// SPDX-License-Identifier: GPL-3.0-or-later
//
// VCA masters and folders in the Mix window (ADR-0048): what the strips and
// the folder cards read, and what a hand on them does. See mix_model.h.
#include <QList>
#include <QStringList>
#include <QVariantMap>
#include <algorithm>

#include "fader_taper.h"
#include "mix_model.h"
#include "session_model.h"

namespace sonora::ui {

namespace {

QVariantMap vca_info(const model::Snapshot& s, const model::Vca& v) {
    const model::VcaMix* m = s.mix.find_vca(v.id);
    const float db = m != nullptr ? m->gain_db : 0.0f;
    QVariantMap out;
    out.insert(QStringLiteral("name"), QString::fromStdString(v.name));
    out.insert(QStringLiteral("gainDb"), static_cast<double>(db));
    out.insert(QStringLiteral("gainText"), QString::fromStdString(gain_text(db)));
    out.insert(QStringLiteral("mute"), m != nullptr && m->mute);
    out.insert(QStringLiteral("count"), static_cast<int>(v.members.size()));
    return out;
}

bool has(const std::vector<model::Id>& list, model::Id id) {
    return std::find(list.begin(), list.end(), id) != list.end();
}

}  // namespace

QVariantList MixModel::vcas() const {
    QVariantList out;
    const model::Snapshot& s = session_.controller().snapshot();
    for (const model::Vca& v : s.edit->vcas) out << vca_info(s, v);
    return out;
}

QStringList MixModel::trackVcas() const {
    QStringList out;
    const model::Snapshot& s = session_.controller().snapshot();
    for (const auto& t : s.edit->tracks) {
        QStringList names;
        for (const model::Vca& v : s.edit->vcas) {
            if (has(v.members, t->id)) names << QString::fromStdString(v.name);
        }
        out << names.join(QStringLiteral(", "));
    }
    return out;
}

QList<int> MixModel::trackFolders() const {
    QList<int> out;
    const EditController& c = session_.controller();
    for (int i = 0; i < session_.trackCount(); ++i) out << c.folder_index(i);
    return out;
}

bool MixModel::folderOpen(int folder) const {
    const auto& folders = session_.controller().snapshot().edit->folders;
    return folder >= 0 && folder < static_cast<int>(folders.size()) &&
           closed_folders_.count(folders[static_cast<std::size_t>(folder)].id) == 0;
}

int MixModel::folderFirst(int folder) const {
    const model::Snapshot& s = session_.controller().snapshot();
    if (folder < 0 || folder >= static_cast<int>(s.edit->folders.size())) return -1;
    const auto& members = s.edit->folders[static_cast<std::size_t>(folder)].members;
    for (std::size_t i = 0; i < s.edit->tracks.size(); ++i) {
        if (has(members, s.edit->tracks[i]->id)) return static_cast<int>(i);
    }
    return -1;
}

QVariantList MixModel::folders() const {
    QVariantList out;
    const model::Snapshot& s = session_.controller().snapshot();
    for (std::size_t f = 0; f < s.edit->folders.size(); ++f) {
        const model::Folder& folder = s.edit->folders[f];
        bool mute = !folder.members.empty();
        bool solo = !folder.members.empty();
        for (const model::Id id : folder.members) {
            const model::TrackMix* m = s.mix.find(id);
            mute = mute && m != nullptr && m->mute;
            solo = solo && m != nullptr && m->solo;
        }
        QVariantMap info;
        info.insert(QStringLiteral("name"), QString::fromStdString(folder.name));
        info.insert(QStringLiteral("count"), static_cast<int>(folder.members.size()));
        info.insert(QStringLiteral("open"), folderOpen(static_cast<int>(f)));
        info.insert(QStringLiteral("mute"), mute);
        info.insert(QStringLiteral("solo"), solo);
        out << info;
    }
    return out;
}

QList<int> MixModel::trackSlots() const {
    QList<int> out;
    const int n = session_.trackCount();
    const QList<int> in_folder = trackFolders();
    QList<bool> placed(session_.controller().snapshot().edit->folders.size(), false);
    int next = 0;
    for (int i = 0; i < n; ++i) {
        const int f = in_folder[i];
        if (f < 0 || folderOpen(f)) {
            out << next++;
            continue;
        }
        // A closed folder is one slot, where its first track was.
        if (!placed[f]) {
            placed[f] = true;
            ++next;
        }
        out << -1;
    }
    return out;
}

int MixModel::slotCount() const {
    int n = 0;
    const QList<int> in_folder = trackFolders();
    QList<bool> placed(session_.controller().snapshot().edit->folders.size(), false);
    for (const int f : in_folder) {
        if (f < 0 || folderOpen(f)) {
            ++n;
        } else if (!placed[f]) {
            placed[f] = true;
            ++n;
        }
    }
    return n;
}

QVariantList MixModel::folderCards() const {
    // The slot is counted among the visible tracks before the folder's first.
    QVariantList out;
    const QVariantList all = folders();
    const QList<int> in_folder = trackFolders();
    int next = 0;
    QList<bool> placed(all.size(), false);
    for (int i = 0; i < in_folder.size(); ++i) {
        const int f = in_folder[i];
        if (f < 0 || folderOpen(f)) {
            ++next;
            continue;
        }
        if (placed[f]) continue;
        placed[f] = true;
        QVariantMap card = all[f].toMap();
        card.insert(QStringLiteral("slot"), next++);
        card.insert(QStringLiteral("folder"), f);
        out << card;
    }
    return out;
}

bool MixModel::vcaResult(bool ok, const std::string& why) {
    session_.reportEdit(ok, std::string());
    if (!ok && !why.empty()) emit session_.heldNotice(QString::fromStdString(why));
    return ok;
}

bool MixModel::newVca(const QString& name) {
    const Selection& s = session_.controller().selection();
    std::string why;
    return vcaResult(session_.controller().vca_new(s.track, s.tracks, name.toStdString(), why), why);
}

bool MixModel::assignVca(int vca) {
    const Selection& s = session_.controller().selection();
    std::string why;
    return vcaResult(session_.controller().vca_assign(vca, s.track, s.tracks, why), why);
}

bool MixModel::removeVca(int vca) {
    std::string why;
    return vcaResult(session_.controller().vca_remove(vca, why), why);
}

void MixModel::vcaDrag(int vca, qreal dy, qreal travel, bool fine) {
    const QVariantList all = vcas();
    if (vca < 0 || vca >= all.size()) return;
    const float db = fader_drag(static_cast<float>(all[vca].toMap().value(QStringLiteral("gainDb")).toDouble()),
                                static_cast<float>(dy), static_cast<float>(travel), fine);
    std::string why;
    session_.reportEdit(session_.controller().set_vca_gain(vca, db, why), why);
}

void MixModel::resetVca(int vca) {
    std::string why;
    session_.reportEdit(session_.controller().set_vca_gain(vca, 0.0f, why), why);
}

bool MixModel::setVcaGainText(int vca, const QString& text) {
    float db = 0.0f;
    std::string why;
    if (!parse_gain_text(text.toStdString(), db, why)) return vcaResult(false, why);
    return vcaResult(session_.controller().set_vca_gain(vca, db, why), why);
}

bool MixModel::toggleVcaMute(int vca) {
    std::string why;
    return vcaResult(session_.controller().toggle_vca_mute(vca, why), why);
}

bool MixModel::newFolder(const QString& name) {
    const Selection& s = session_.controller().selection();
    std::string why;
    return vcaResult(session_.controller().folder_new(s.track, s.tracks, name.toStdString(), why),
                     why);
}

bool MixModel::dissolveFolder(int folder) {
    const int first = folderFirst(folder);
    std::string why;
    if (first < 0) {
        why = "there is no such folder";
        return vcaResult(false, why);
    }
    return vcaResult(session_.controller().folder_dissolve(first, why), why);
}

void MixModel::toggleFolderOpen(int folder) {
    const auto& folders = session_.controller().snapshot().edit->folders;
    if (folder < 0 || folder >= static_cast<int>(folders.size())) return;
    const model::Id id = folders[static_cast<std::size_t>(folder)].id;
    if (!closed_folders_.erase(id)) closed_folders_.insert(id);
    emit structureChanged();
}

bool MixModel::toggleFolderMute(int folder) {
    const int first = folderFirst(folder);
    std::string why;
    if (first < 0) return vcaResult(false, "there is no such folder");
    return vcaResult(session_.controller().folder_toggle(first, false, why), why);
}

bool MixModel::toggleFolderSolo(int folder) {
    const int first = folderFirst(folder);
    std::string why;
    if (first < 0) return vcaResult(false, "there is no such folder");
    return vcaResult(session_.controller().folder_toggle(first, true, why), why);
}

}  // namespace sonora::ui
