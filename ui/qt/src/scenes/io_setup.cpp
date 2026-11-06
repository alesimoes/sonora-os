// SPDX-License-Identifier: GPL-3.0-or-later
//
// Setup > I/O Setup on the fake engine's UMC1820 stand-in: its 18 inputs
// named for a drum-and-band tracking date, the tracks moved onto them by
// name (Hi-Hat onto "Hi-Hat", input 4, past the second snare mic), the
// input list open on Hi-Hat, and Talkback left on an input 19 the
// interface does not have, which its row says in the alarm colour.
#include <QQuickItem>

#include "io_setup_model.h"
#include "screenshot_scene.h"
#include "sonora/model/operations.h"

namespace sonora::ui {
namespace {

const char* const kInputs[] = {"Kick In",  "Snare Top", "Snare Btm", "Hi-Hat",  "Tom 1",  "Tom 2",
                               "OH L",     "OH R",      "Room",      "Bass DI", "Bass Amp", "Gtr L",
                               "Gtr R",    "Keys L",    "Keys R",    "Vox",     "BV",     "Talkback"};

void setup_io(ShotContext& c) {
    QQuickItem* root = c.view.rootObject();
    auto* io = root->property("io").value<IoSetupModel*>();
    if (io == nullptr) return;
    // Names of the scene's own, never this machine's file.
    io->setNamesFile(std::string());
    io->refresh();
    std::string why;
    for (int i = 0; i < 18; ++i) io->rename(IoSide::Input, i, kInputs[i], why);
    io->rename(IoSide::Output, 0, "Mon L", why);
    io->rename(IoSide::Output, 1, "Mon R", why);
    // Hi-Hat to BV 2 one input up, past the second snare mic; then BV 2
    // back down, sharing BV 1's mic.
    const int tracks = c.session.trackCount();
    for (int row = 2; row < 17 && row < tracks; ++row) io->pickInput(row, row + 1, why);
    if (tracks > 16) io->pickInput(16, 16, why);
    // Talkback on an input this interface does not have: only a document
    // edit can put it there, as a session from a bigger interface would.
    if (tracks > 17) {
        const model::Snapshot& s = c.session.controller().snapshot();
        const model::TrackMix* m = s.mix.find(s.edit->tracks[17]->id);
        if (m != nullptr) {
            model::TrackMix moved = *m;
            moved.input = {18};
            c.session.reportEdit(
                c.session.controller().document().submit(model::SetMix{moved}, why), why);
        }
    }
    io->setOpen(true);
    if (auto* win = root->findChild<QQuickItem*>(QStringLiteral("ioSetupWindow")))
        win->setProperty("picking", 2);
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("io-setup");
    s.take = false;
    s.apply = [](ShotContext& c) { when_open(c, setup_io); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
