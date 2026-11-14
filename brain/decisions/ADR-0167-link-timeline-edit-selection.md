# ADR-0167: Link Timeline and Edit Selection, non-contiguous track selection

Status: accepted (2026-11-13). Track: ui.editing.selection.

## Summary

`SelectionLink` (`ui/qt/src/selection_link.h`) is interface state: a `linked`
flag (default on, as in Pro Tools), the timeline's own range, and the set of
tracks added with Ctrl-click. When linked, the edit selection's range and the
timeline (loop/playback) range are one range, mirrored both ways; unlinked,
each moves alone. No contract, protocol or session-format change.

## Decisions

- **State is the operator's, not the session's.** Only `linked` is persisted,
  in `selection-link.json` beside the window layouts
  (`$SONORA_SELECTION_LINK_FILE`, else `$XDG_CONFIG_HOME/sonora/`). A range
  and a track set belong to the session being edited and are not kept.
  A missing file loads as the default; a file that does not say true or false
  is refused with a reason and leaves the state as it was.
- **Plain C++, header-only.** `EditController` lives in a Qt-free target and
  holds a `SelectionLink*` (`set_link`, null allowed). `select_range` mirrors
  into the link; `apply_timeline_range` is the way back; `clamp_selection`
  prunes Ctrl-click tracks that no longer exist.
- **Toggle.** `LinkSelectionButton.qml` handles Shift+/ with a `Shortcut`.
  The keymap line (`keymaps/protools.json`) and the registration of the QML
  file in `ui/qt/CMakeLists.txt` and its placement in the edit toolbar are
  follow-ups, because other features hold those files.

## Consequences

Until the follow-up, the button file is not compiled into the resources and
the link is not yet wired to the transport's loop range.
