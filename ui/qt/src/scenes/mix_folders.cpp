// SPDX-License-Identifier: GPL-3.0-or-later
//
// Folders (ADR-0042) in the Mix window: "Guitars" closed into one narrow
// card where its first track was, its three strips gone, with a mute, a
// solo, Open and Ungroup; "Keys" open beside it, wearing its tag.
#include <QQuickItem>
#include <cstdio>

#include "mix_model.h"
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

bool run(ShotContext& c, const char* command, const char* argument) {
    if (c.model.invokeWith(QString::fromLatin1(command), -1, QString::fromUtf8(argument)))
        return true;
    std::fprintf(stderr, "sonora-ui: %s: %s\n", command, qPrintable(c.model.statusText()));
    return false;
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-folders");
    s.window = QStringLiteral("mix");
    s.take = false;
    s.apply = [](ShotContext& c0) {
        when_open(c0, [](ShotContext& c) {
            auto* mixer = c.view.rootObject()->property("mixer").value<MixModel*>();
            if (mixer == nullptr) return;
            EditController& edit = c.session.controller();
            edit.select_range(4, 0, 0, 3);
            if (!run(c, "folder.new", "Guitars")) return;
            edit.select_range(9, 0, 0, 3);
            if (!run(c, "folder.new", "Keys")) return;
            mixer->toggleFolderMute(0);
            mixer->toggleFolderOpen(0);
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
