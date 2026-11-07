// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Helpers shared by the session_model*.cpp files (SessionModel is split by
// domain; this is not a public header).

#include <QList>
#include <QString>
#include <string>

#include "sonora/model/model.h"

namespace sonora::ui {

inline QString q(const std::string& s) { return QString::fromStdString(s); }

// MixState::tracks is keyed by id, in no display order, and keeps the
// entries of undone AddTracks: indexing it by strip showed one track's
// fader on another's strip once tracks were reordered or one was undone.
template <typename T, typename F>
QList<T> per_strip(const model::Snapshot& s, F field) {
    QList<T> out;
    for (const auto& t : s.edit->tracks) {
        const model::TrackMix* m = s.mix.find(t->id);
        out << (m != nullptr ? field(*m) : field(model::TrackMix{}));
    }
    return out;
}

inline const model::TrackMix* strip_mix(const model::Snapshot& s, int track) {
    if (track < 0 || track >= static_cast<int>(s.edit->tracks.size())) return nullptr;
    return s.mix.find(s.edit->tracks[static_cast<std::size_t>(track)]->id);
}

}  // namespace sonora::ui
