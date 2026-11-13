# ADR-0088: Launching a plug-in's own editor from the UI

Status: Accepted (recommended defaults, 2026-11-08; BACKLOG `plugins.host.editors`).
Builds on: ADR-0012 (amendment of 2026-11-03: the host opens VST3 editors; `OpenEditor`, `EditorOpened`, `EditorFailed`, `EditorClosed` in `contracts/plugin/include/sonora/plugin/host_control.h`), ADR-0010 (kiosk compositor, parentless toplevels placed by labwc), ADR-0007 §1 and update 1 item 6 (module graph, the opaque parent-window token).
Scope: the contract only. `engine/editor_service` (header-only, std only) carries the interface and a fake. The engine wiring, the wire messages and the UI are the follow-up feature `plugins.host.editors.wire`.

## Summary

The UI asks the engine to open the editor of an insert; the engine asks the
plug-in host that runs it. The UI never talks to a host. There is one editor
per insert: a second request raises the window that is already open. An editor
is `Closed`, `Opening`, `Open` or `Failed` with the host's reason; an editor
that cannot open shows that reason and the UI offers the generic editor,
never a blank window. The window is a parentless toplevel (empty parent token)
on the appliance and under WSLg. It lives and dies with the host. Values moved
in it reach the document through the existing `ParamTouched` / gesture path.
The follow-up adds `OpenPluginEditor{track,slot}`,
`ClosePluginEditor{track,slot}` and the event
`PluginEditorState{track,slot,state,reason}` to contracts/api, with a protocol
number the coordinator reserves when it dispatches that work; this ADR and
its PR carry no wire change.

## Context

The host side of editors is on main (#322, #339): `plugins/host` opens a VST3
editor as an X11 window (`editor_x11.cpp`), answers `OpenEditor` with
`EditorOpened` or `EditorFailed{text}`, sends `EditorClosed` exactly once for
every `EditorOpened` (reason `Requested`, `ByOperator` or `HostStopping`), and
`HostSupervisor` tracks `open_editors_`. Nothing lets the operator open one:
contracts/api has no command for it, and the UI has no state to draw.

Three facts shape the interface:

- Plug-in hosts run out of process (ADR-0012). Only the engine knows which
  host runs the insert at `{track, slot}`, which instance id it carries, and
  whether that host is alive. A UI that spoke to hosts would have to learn
  all three and would race the engine's make-before-break swaps.
- The host already refuses a second `OpenEditor` for an open instance with
  `EditorFailed` and raises the window. The engine must not show that refusal
  to the operator as a failure.
- Many editors cannot open: no `DISPLAY`, an LV2 UI (the host opens VST3
  editors only), a host built without X11. The operator has to learn why, and
  still has to be able to edit the plug-in.

## Decision

1. **The UI asks the engine; the engine asks the host.** The UI names an
   insert by `{track, slot}`. The engine resolves it to the host and instance
   and sends `OpenEditor` / `CloseEditor`. The UI never holds a host address,
   an instance id or a window id.
2. **One editor per insert.** A request to open an editor that is `Opening`
   or `Open` does not start a second one: the engine raises the existing
   window (the host's answer to a second `OpenEditor`) and the state stays
   `Open`. The host's `EditorFailed` for that case is not reported as a
   failure.
3. **Four states.** `Closed`, `Opening`, `Open`, `Failed`. `Failed` carries the
   reason text from `EditorFailed`, unchanged. A failed editor shows that
   reason, and the UI offers the generic (parameter-list) editor in its place;
   it never shows a blank window. `Failed` is not sticky: a new open request
   tries again.
4. **Parent token.** The token in `OpenEditor` is empty on the appliance and
   under WSLg: the editor is a parentless toplevel, placed by the compositor
   (labwc on the appliance, ADR-0010; the WSLg compositor on Windows).
5. **The editor lives and dies with the host.** A host crash, a host stop, or
   the insert's removal from the chain reports `Closed` for every editor that
   host had, whether it was `Opening` or `Open`. `EditorClosed` for any reason
   (`Requested`, `ByOperator`, `HostStopping`) reports `Closed`. Closing an
   editor that is already `Closed` or `Failed` is ignored.
6. **No new value path.** A value moved in the editor reaches the document
   through the existing `ParamTouched` / `GestureBegin` / `GestureEnd` path and
   `EditorClosed{moved}` (host_control.h, "Gestures"). The editor service only
   tracks window state; it never carries a parameter value.
7. **Wire, for the follow-up.** contracts/api gains the commands
   `OpenPluginEditor{track, slot}` and `ClosePluginEditor{track, slot}` and the
   broadcast event `PluginEditorState{track, slot, state, reason}`, sent on
   every state change to every client, so a second client sees the same
   state. The protocol version is reserved by the coordinator when it
   dispatches `plugins.host.editors.wire`; this PR changes no wire.

### Contract

`engine/editor_service/include/sonora/editor_service/plugin_editor_service.h`,
header-only INTERFACE library `sonora_editor_service`, std only:

- `enum class EditorState { Closed, Opening, Open, Failed }`
- `struct EditorStatus { EditorState state; std::string reason; }`
- `class PluginEditorService` with `open(track, slot) -> EditorStatus`,
  `close(track, slot)`, `status(track, slot) -> EditorStatus` and
  `set_listener(std::function<void(track, slot, const EditorStatus&)>)`. The
  listener is called on every state change and only on a change, on the
  control thread. Nothing here runs on the audio thread.
- `FakePluginEditorService`: the same rules in memory, with
  `script_failure(track, slot, reason)`, `hold_opening(track, slot)`,
  `complete_open(track, slot)`, `host_stopped(track)` and
  `insert_removed(track, slot)` to drive the cases the tests and the UI need.
- Tests: `engine/editor_service/tests/test_plugin_editor_service.cpp`
  (ctest `plugin_editor_service`, label `control`).

## Rationale

- Routing through the engine keeps the module graph as it is (ADR-0007 §1: a
  UI does not reach a host) and keeps one owner for "which host runs this
  insert", which already changes under the UI during a swap.
- One editor per insert matches what the host does and what the operator
  expects from a hardware-like faceplate: pressing the button again brings it
  to the front.
- A `Failed` state with the host's own text, plus the generic editor, turns
  every refusal into something the operator can act on. A blank window is
  indistinguishable from a hung plug-in.
- Parentless toplevels are what ADR-0010 already decided for the appliance,
  and the only mode that does not depend on X11 reparenting under a Wayland
  compositor.
- Reusing the gesture path means one route from plug-in to document: undo,
  automation write and every client's view already follow it.

## Consequences

- `plugins.host.editors.wire` implements `PluginEditorService` in
  engine/control on top of `HostSupervisor`, adds the three messages to
  contracts/api with a reserved protocol number, and adds the UI button,
  state and generic-editor fallback. It owns
  `engine/control/CMakeLists.txt`, `engine_control_server.cpp` and
  `contracts/api/CMakeLists.txt`, which `interchange.stems` holds now; it is
  planned after that merges.
- The engine must map `EditorFailed` for an instance already `Opening`/`Open`
  to "raised", not to `Failed`.
- The UI keeps no editor state of its own beyond the last
  `PluginEditorState` it received.
- Unverified: the window's behaviour under WSLg and labwc (placement, focus,
  raise on the second request). The researcher measures it on both before
  the wire feature ships.

## Alternatives rejected

- **Embedding the plug-in window in the Qt UI.** It needs X11 reparenting of a
  foreign window into a Qt widget, which is fragile under Wayland labwc
  (ADR-0010) and ties the UI process's lifetime to third-party drawing code.
- **Generic editor only.** Safe, but many plug-ins are unusable without their
  own editor (curves, displays, preset browsers); the generic editor stays as
  the fallback, not the only option.
- **Polling plug-in or editor state.** The host already pushes `EditorOpened`,
  `EditorFailed` and `EditorClosed`; polling adds latency and load and still
  misses a window closed between two polls. State is event-driven end to end.
- **The UI talking to the host directly.** Breaks the module graph and races
  the engine's host swaps (Context).

## Amendment 2026-11-13: the wire (protocol 70)

Decision 7's messages, as `plugins.host.editors.rest` puts them on the wire.
The lead reserved protocol **70**; a peer of 69 or lower is refused at Hello.

- They are words of the existing messages, not wire types of their own, so
  the codec, the reply and the command map stay the ones every command uses.
  `contracts/api/include/sonora/api/plugin_editor.h` declares the structs and
  the mapping; `contracts/api/src/plugin_editor_wire.cpp` implements it.
- `OpenPluginEditor{track, slot}` is the catalog's `plugin.editor.open`
  (ADR-0012 §8): `CommandRequest::target` the track index, argument the slot
  in decimal, `0 <= slot < kInsertSlots`. `ClosePluginEditor` is
  `plugin.editor.close`, the same. ADR-0012's argument
  `"<slot> <parent token>"` loses its token: the interface holds no window
  (§1), and the engine sends the host an empty one (§4). Nothing ran the old
  form, so no client changes. The reply is Ok once the request is taken; the outcome is the event.
- `PluginEditorState{track, slot, state, reason}` is `EventKind::PluginEditor`
  (11): `track` the track index, `insert` the slot, `flags` the
  `PluginEditorStatus` (`Closed 0`, `Opening 1`, `Open 2`, `Failed 3`, as
  `editor_service::EditorState`), `text` the host's reason on `Failed` and
  empty otherwise. The codec refuses a state past `Failed`; `from_event`
  also refuses a negative track, a slot out of range and a reason on any
  other state.
- A client that attaches later is sent every editor that is not `Closed`.
  Opening a session reports `Closed` for every editor of the last one,
  `Failed` included, so no notice outlives the insert it names.
- `FakeEngine` runs the two commands with `Config::plugin_editors` (off by
  default until engine/control installs them, so `engine_commands()` still
  matches it), with `script_editor_failure`, `hold_editor_opening`,
  `complete_editor_open` and `editor_host_stopped`.
- Test: `contracts/api/tests/test_plugin_editor_wire.cpp` (ctest
  `api_plugin_editor_wire`, label `contracts`).
