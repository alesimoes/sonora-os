// SPDX-License-Identifier: GPL-3.0-or-later
//
// VCA masters and folders (ADR-0042) on the Edit controller. Making,
// re-membering and removing either is one operation and one undo step; a
// VCA's fader and mute are SetVcaMix, saved and not undone; a folder's mute
// and solo are one Compound of SetMix over its members, so the engine hears
// them change together.
#include <algorithm>
#include <cmath>

#include "edit_controller.h"
#include "fader_taper.h"

namespace sonora::ui {

using namespace model;

namespace {

std::string trim(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}

template <typename T>
std::string free_name(const std::vector<T>& list, std::string name, const char* base) {
    name = trim(std::move(name));
    if (!name.empty()) return name;
    for (int n = 1;; ++n) {
        name = std::string(base) + " " + std::to_string(n);
        if (std::none_of(list.begin(), list.end(), [&](const T& x) { return x.name == name; })) {
            return name;
        }
    }
}

template <typename T>
bool name_taken(const std::vector<T>& list, const std::string& name) {
    return std::any_of(list.begin(), list.end(), [&](const T& x) { return x.name == name; });
}

}  // namespace

bool EditController::selected_ids(int first, int count, std::vector<Id>& out,
                                  std::string& why) const {
    const auto& tracks = snapshot().edit->tracks;
    if (first < 0 || first >= static_cast<int>(tracks.size())) {
        why = first < 0 ? "no track is selected" : "there is no track " + std::to_string(first + 1);
        return false;
    }
    const int last = std::min(first + std::max(count, 1), static_cast<int>(tracks.size()));
    for (int i = first; i < last; ++i) out.push_back(tracks[static_cast<std::size_t>(i)]->id);
    return true;
}

int EditController::folder_index(int index) const {
    const TrackEdit* t = track(index);
    if (t == nullptr) return -1;
    const auto& folders = snapshot().edit->folders;
    for (std::size_t f = 0; f < folders.size(); ++f) {
        const auto& m = folders[f].members;
        if (std::find(m.begin(), m.end(), t->id) != m.end()) return static_cast<int>(f);
    }
    return -1;
}

bool EditController::vca_new(int first, int count, const std::string& name_in, std::string& why) {
    std::vector<Id> members;
    if (!selected_ids(first, count, members, why)) return false;
    const auto& vcas = snapshot().edit->vcas;
    if (vcas.size() >= kMaxVcas) {
        why = "a session holds at most " + std::to_string(kMaxVcas) + " VCA masters";
        return false;
    }
    const std::string name = free_name(vcas, name_in, "VCA");
    if (name_taken(vcas, name)) {
        why = "another VCA is already named \"" + name + "\"";
        return false;
    }
    SetVca op;
    op.vca = snapshot().next_id;
    op.name = name;
    op.members = std::move(members);
    return submit(op, why);
}

bool EditController::vca_assign(int vca, int first, int count, std::string& why) {
    const auto& vcas = snapshot().edit->vcas;
    if (vca < 0 || vca >= static_cast<int>(vcas.size())) {
        why = "there is no such VCA";
        return false;
    }
    SetVca op;
    op.vca = vcas[static_cast<std::size_t>(vca)].id;
    op.name = vcas[static_cast<std::size_t>(vca)].name;
    if (!selected_ids(first, count, op.members, why)) {
        why += "; select the tracks the VCA should hold";
        return false;
    }
    return submit(op, why);
}

bool EditController::vca_remove(int vca, std::string& why) {
    const auto& vcas = snapshot().edit->vcas;
    if (vca < 0 || vca >= static_cast<int>(vcas.size())) {
        why = "there is no such VCA";
        return false;
    }
    return submit(RemoveVca{vcas[static_cast<std::size_t>(vca)].id}, why);
}

namespace {

VcaMix vca_mix_of(const Snapshot& s, const Vca& v) {
    if (const VcaMix* m = s.mix.find_vca(v.id)) return *m;
    VcaMix m;
    m.vca = v.id;
    return m;
}

}  // namespace

bool EditController::set_vca_gain(int vca, float db, std::string& why) {
    const auto& vcas = snapshot().edit->vcas;
    if (vca < 0 || vca >= static_cast<int>(vcas.size())) {
        why = "there is no such VCA";
        return false;
    }
    if (!std::isfinite(db)) {
        why = "a gain must be a number of dB";
        return false;
    }
    VcaMix m = vca_mix_of(snapshot(), vcas[static_cast<std::size_t>(vca)]);
    m.gain_db = std::clamp(db, std::min(kFaderSilenceDb, m.gain_db), std::max(kFaderMaxDb, m.gain_db));
    return submit(SetVcaMix{m}, why);
}

bool EditController::toggle_vca_mute(int vca, std::string& why) {
    const auto& vcas = snapshot().edit->vcas;
    if (vca < 0 || vca >= static_cast<int>(vcas.size())) {
        why = "there is no such VCA";
        return false;
    }
    VcaMix m = vca_mix_of(snapshot(), vcas[static_cast<std::size_t>(vca)]);
    m.mute = !m.mute;
    return submit(SetVcaMix{m}, why);
}

bool EditController::folder_new(int first, int count, const std::string& name_in, std::string& why) {
    std::vector<Id> members;
    if (!selected_ids(first, count, members, why)) return false;
    const auto& folders = snapshot().edit->folders;
    if (folders.size() >= kMaxFolders) {
        why = "a session holds at most " + std::to_string(kMaxFolders) + " folders";
        return false;
    }
    for (int i = first; i < first + static_cast<int>(members.size()); ++i) {
        const int f = folder_index(i);
        if (f >= 0) {
            why = "\"" + track(i)->name + "\" is already in folder \"" +
                  folders[static_cast<std::size_t>(f)].name +
                  "\"; a track is in one folder at most. Dissolve that folder first. "
                  "Nothing was put in a folder.";
            return false;
        }
    }
    const std::string name = free_name(folders, name_in, "Folder");
    if (name_taken(folders, name)) {
        why = "another folder is already named \"" + name + "\"";
        return false;
    }
    SetFolder op;
    op.folder = snapshot().next_id;
    op.name = name;
    op.members = std::move(members);
    return submit(op, why);
}

bool EditController::folder_dissolve(int index, std::string& why) {
    const int f = folder_index(index);
    if (f < 0) {
        why = track(index) == nullptr ? "no track is selected"
                                      : "\"" + track(index)->name + "\" is not in a folder";
        return false;
    }
    return submit(RemoveFolder{snapshot().edit->folders[static_cast<std::size_t>(f)].id}, why);
}

bool EditController::folder_toggle(int index, bool solo, std::string& why) {
    const int f = folder_index(index);
    if (f < 0) {
        why = track(index) == nullptr ? "no track is selected"
                                      : "\"" + track(index)->name + "\" is not in a folder";
        return false;
    }
    const Folder& folder = snapshot().edit->folders[static_cast<std::size_t>(f)];
    bool all = true;
    for (const Id id : folder.members) {
        const TrackMix* m = snapshot().mix.find(id);
        all = all && m != nullptr && (solo ? m->solo : m->mute);
    }
    std::vector<Member> ops;
    for (const Id id : folder.members) {
        const TrackMix* m = snapshot().mix.find(id);
        if (m == nullptr || (solo ? m->solo : m->mute) == !all) continue;
        TrackMix next = *m;
        (solo ? next.solo : next.mute) = !all;
        ops.push_back(SetMix{next});
    }
    if (ops.empty()) return true;
    return submit_compound(std::move(ops), solo ? "Folder Solo" : "Folder Mute", "changed", why);
}

}  // namespace sonora::ui
