// SPDX-License-Identifier: GPL-3.0-or-later
#include "editors.h"

namespace sonora::plugin_host {

bool Editors::open(uint32_t instance, const std::string& parent_token, std::string& error) {
    if (instance >= windows_.size()) {
        error = "no instance " + std::to_string(instance);
        return false;
    }
    if (windows_[instance]) {
        windows_[instance]->raise();
        error = "the editor of instance " + std::to_string(instance) + " is already open";
        return false;
    }
    if (!display_) {
        // Not kept on failure: a display that comes later (Xwayland is
        // started lazily, ADR-0010) is tried at the next OpenEditor.
        display_ = EditorDisplay::open(error);
        if (!display_) return false;
    }
    std::unique_ptr<EditorWindow> window = display_->create_window(chain_.info(instance).name, parent_token, error);
    if (!window) return false;
    if (!chain_.processor(instance).open_editor(*display_, *window, error)) return false;
    window->show();
    windows_[instance] = std::move(window);
    return true;
}

bool Editors::close(uint32_t instance) {
    if (!is_open(instance)) return false;
    // The view leaves its parent before the parent goes.
    chain_.processor(instance).close_editor();
    windows_[instance].reset();
    return true;
}

std::vector<uint32_t> Editors::open_instances() const {
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < windows_.size(); ++i)
        if (windows_[i]) out.push_back(i);
    return out;
}

void Editors::close_all() {
    for (uint32_t i = 0; i < windows_.size(); ++i) close(i);
}

uint64_t Editors::dispatch(std::vector<uint32_t>& closed) {
    if (!display_) return 0;
    const uint64_t due = display_->dispatch();
    for (uint32_t i = 0; i < windows_.size(); ++i) {
        EditorWindow* w = windows_[i].get();
        if (!w) continue;
        if (w->close_requested()) {
            close(i);
            closed.push_back(i);
            continue;
        }
        uint32_t width = 0, height = 0;
        if (w->take_resize(width, height)) {
            uint32_t took_w = width, took_h = height;
            chain_.processor(i).editor_resized(took_w, took_h);
            // An editor that keeps its size, or constrains it, gets the
            // window put back to what it drew.
            if (took_w != width || took_h != height) w->set_size(took_w, took_h, w->resizable());
        }
    }
    return due;
}

}  // namespace sonora::plugin_host
