// SPDX-License-Identifier: GPL-3.0-or-later
//
// Window configurations (Pro Tools' Window > Configuration; Logic's
// screensets): which of the floating windows are open and where, and which
// of Edit and Mix is in front, kept in nine numbered slots like memory
// locations. Interface state, never the session's: a layout fits the
// display it was made on, so it is saved per machine, in a file of its own
// beside the preferences, and no session format carries it.
//
// Qt Core only, no display: the tests drive it directly.
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <map>

namespace sonora::ui {

struct FloatingWindow {
    bool open = false;
    // Where the operator (or the default) put it; `placed` false until the
    // interface has reported a position.
    bool placed = false;
    int x = 0;
    int y = 0;
};

struct WindowLayout {
    QString view = QStringLiteral("edit");   // "edit" or "mix"
    std::map<QString, FloatingWindow> windows;
};

// $SONORA_WINDOW_LAYOUTS_FILE, else $XDG_CONFIG_HOME/sonora/window-layouts.json.
QString default_layouts_file();

class WindowLayouts {
public:
    static constexpr int kSlots = 9;
    // The floating windows a layout covers. Windows that arrive later
    // (Bounce, Spot, plug-in editors) are added here and nowhere else.
    static const QStringList& ids();
    static bool known(const QString& id);

    // What is on screen now.
    const WindowLayout& current() const { return current_; }
    void setView(const QString& view);
    void setOpen(const QString& id, bool open);
    void place(const QString& id, int x, int y);

    bool stored(int slot) const;
    // Copies the current arrangement into `slot` (1..9). False, with `why`,
    // for a slot outside that.
    bool store(int slot, QString& why);
    // Makes the slot's arrangement the current one. A window the slot never
    // had a position for keeps the one it has. False, with `why`, for an
    // empty slot: recalling nothing must not rearrange the screen.
    bool recall(int slot, QString& why);
    // "Mix, 3 floating", for the list.
    QString summary(int slot) const;

    QByteArray toJson() const;
    // Replaces the slots. A file that does not parse leaves them as they
    // were and says why; an unknown window id inside it is dropped, since a
    // newer build may have written it.
    bool fromJson(const QByteArray& bytes, QString& why);
    // Loads and saves `path`; save creates the folder. A missing file is
    // not a fault on load. False, with `why`, otherwise.
    bool load(const QString& path, QString& why);
    bool save(const QString& path, QString& why) const;

private:
    WindowLayout current_;
    std::map<int, WindowLayout> slots_;
};

}  // namespace sonora::ui
