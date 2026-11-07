// SPDX-License-Identifier: GPL-3.0-or-later
//
// apply() for the family GroupOp (ADR-0059):
// mix and edit groups, VCAs and folders.
#include "apply_detail.h"

namespace sonora::model::apply_detail {

bool Applier::operator()(const SetGroup& op) {
    const auto& groups = before_.edit->groups;
    const auto at = std::find_if(groups.begin(), groups.end(),
                                 [&](const Group& g) { return g.id == op.group; });
    if (at == groups.end() && !fresh(op.group)) return false;
    auto edit = std::make_shared<EditState>(*before_.edit);
    Group g;
    g.id = op.group;
    g.name = op.name;
    g.members = op.members;
    g.attrs = op.attrs;
    g.relative = op.relative;
    g.active = op.active;
    if (at == groups.end()) {
        edit->groups.push_back(std::move(g));
    } else {
        edit->groups[static_cast<std::size_t>(at - groups.begin())] = std::move(g);
    }
    std::string where;
    const std::string why = detail::group_problem(*edit, where);
    if (!why.empty()) return fail("group" + where + ": " + why);
    after_ = before_;
    after_.edit = std::move(edit);
    if (at == groups.end()) after_.next_id = op.group + 1;
    return true;
}

bool Applier::operator()(const RemoveGroup& op) {
    const auto& groups = before_.edit->groups;
    const auto at = std::find_if(groups.begin(), groups.end(),
                                 [&](const Group& g) { return g.id == op.group; });
    if (at == groups.end()) return fail("group " + str(op.group) + " does not exist");
    auto edit = std::make_shared<EditState>(*before_.edit);
    edit->groups.erase(edit->groups.begin() + (at - groups.begin()));
    after_ = before_;
    after_.edit = std::move(edit);
    return true;
}

bool Applier::operator()(const SetVca& op) {
    auto edit = std::make_shared<EditState>(*before_.edit);
    const bool is_new = std::none_of(edit->vcas.begin(), edit->vcas.end(),
                                     [&](const Vca& v) { return v.id == op.vca; });
    if (!set_members(edit->vcas, op.vca, op)) return false;
    std::string where;
    const std::string why = detail::vca_problem(*edit, where);
    if (!why.empty()) return fail("VCA" + where + ": " + why);
    after_ = before_;
    after_.edit = std::move(edit);
    if (is_new) after_.next_id = op.vca + 1;
    return true;
}

bool Applier::operator()(const RemoveVca& op) {
    auto edit = std::make_shared<EditState>(*before_.edit);
    const auto at = std::find_if(edit->vcas.begin(), edit->vcas.end(),
                                 [&](const Vca& v) { return v.id == op.vca; });
    if (at == edit->vcas.end()) return fail("VCA " + str(op.vca) + " does not exist");
    edit->vcas.erase(at);
    after_ = before_;
    after_.edit = std::move(edit);
    return true;
}

bool Applier::operator()(const SetVcaMix& op) {
    const auto& vcas = before_.edit->vcas;
    if (std::none_of(vcas.begin(), vcas.end(), [&](const Vca& v) { return v.id == op.mix.vca; })) {
        return fail("VCA " + str(op.mix.vca) + " does not exist");
    }
    std::string where;
    const std::string why = detail::vca_mix_problem(op.mix, where);
    if (!why.empty()) return fail("VCA " + str(op.mix.vca) + "." + where + ": " + why);
    after_ = before_;
    auto& list = after_.mix.vcas;
    const auto at = std::find_if(list.begin(), list.end(),
                                 [&](const VcaMix& m) { return m.vca == op.mix.vca; });
    if (at == list.end()) {
        list.push_back(op.mix);
    } else {
        *at = op.mix;
    }
    return true;
}

bool Applier::operator()(const SetFolder& op) {
    auto edit = std::make_shared<EditState>(*before_.edit);
    const bool is_new = std::none_of(edit->folders.begin(), edit->folders.end(),
                                     [&](const Folder& f) { return f.id == op.folder; });
    if (!set_members(edit->folders, op.folder, op)) return false;
    std::string where;
    const std::string why = detail::folder_problem(*edit, where);
    if (!why.empty()) return fail("folder" + where + ": " + why);
    after_ = before_;
    after_.edit = std::move(edit);
    if (is_new) after_.next_id = op.folder + 1;
    return true;
}

bool Applier::operator()(const RemoveFolder& op) {
    auto edit = std::make_shared<EditState>(*before_.edit);
    const auto at = std::find_if(edit->folders.begin(), edit->folders.end(),
                                 [&](const Folder& f) { return f.id == op.folder; });
    if (at == edit->folders.end()) return fail("folder " + str(op.folder) + " does not exist");
    edit->folders.erase(at);
    after_ = before_;
    after_.edit = std::move(edit);
    return true;
}

}  // namespace sonora::model::apply_detail
