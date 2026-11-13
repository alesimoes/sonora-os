# ADR-0158: Zoomer and Scrubber as pure view tools

Status: accepted (architect, 2026-11-12)

## Context
Smart, Trim, Select, Grab and Pencil exist (`Tool` in edit_controller.h). The
Zoomer and the Scrubber do not. Neither changes the session: the Zoomer
changes the view, the Scrubber moves the playhead and plays audio at the
pointer. edit_controller.{h,cpp}, EditWindow.qml and ui/qt/CMakeLists.txt are
held by other active features.

## Decision
1. New module `ui/edit_tools` (no Qt, depends on nothing): `zoom_rect(x0,y0,x1,y1,view) -> view'`
   (drag a box: time range and track range to fit it, clamped to the
   session and to the minimum fpp), `zoom_click(view, x, out)` (a click zooms
   about x by a fixed step, Alt-click out), `scrub_rate(dx_pixels, dt_us, fpp, rate) -> signed speed`
   (clamped to +-4x, dead zone under 2 px) and `scrub_position`.
2. The Zoomer never sends an engine command; the Scrubber uses the
   existing transport locate and a bounded-speed audition, UI-side only
   (the audio thread never receives a UI push, ADR-0006). No new wire message.
3. Hooked with a guarded `add_subdirectory` at the top of
   `ui/keymap/CMakeLists.txt` (the root CMake and ui/qt's are held); label `ui`.
4. The Qt binding (two more `Tool` values, buttons Z and S, keys, pointer
   handlers) lands as a second PR once edit_controller.h/.cpp and
   EditWindow.qml are free.

## Rationale
Pure functions are testable without a display and without wall-clock waits.
The view tools touch no session, so a mistake cannot lose a take.

## Consequences
The feature ships in two PRs; the first is marked partial. The hook in
ui/keymap moves to the root when ui/qt's CMake frees up.

## Alternatives rejected
- Put the logic in edit_controller.cpp: held by ui.editing.modes, and that file is already large.
- Wait for every held file: leaves the slot idle for days.
- Engine-side scrub: needs a protocol bump for a UI gesture.
