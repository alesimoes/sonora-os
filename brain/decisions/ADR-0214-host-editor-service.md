# ADR-0214: HostEditorService, the editor service over the host control channel

Status: Accepted (recommended default, 2026-11-14; BACKLOG `plugins.host.editors`).
Builds on: ADR-0088 (the contract), ADR-0012 (HostControl OpenEditor, EditorOpened, EditorFailed, EditorClosed).

## Summary

`HostEditorService` implements `PluginEditorService` in `engine/editor_service`.
It talks to the host through `EditorHostLink` (send OpenEditor / CloseEditor for
a track), so tests use a fake and the follow-up binds it to `HostSupervisor`.
Host events are fed in with `on_editor_*`, `on_host_stopped`, `on_insert_removed`.
Control thread only. The instance number in the host is the slot index.
Deviation from the task text: the module links only `contracts/plugin` (the
message structs), not `engine/plugin_client`; the supervisor binding belongs to
the wire follow-up, which avoids a heavy edge now.

## Mapping

| Event | EditorState |
|---|---|
| `open()`, none live, link sent | Opening (request id recorded) |
| `open()`, Opening or Open | unchanged, nothing sent (raise) |
| `open()`, no host for the track | Failed, "no plug-in host is running for this track" |
| EditorOpened (matching request) | Open |
| EditorFailed (matching request, while Opening) | Failed, reason = host text unchanged |
| EditorClosed (any reason) | Closed, if Opening or Open |
| `close()` while live | CloseEditor sent, Closed at once |
| `close()` while Closed or Failed | ignored |
| host crash or stop | every live editor of the track Closed |
| insert removed from chain | its editor Closed |

The parent token is always empty (parentless toplevel). Stale replies (wrong
request id or state) are ignored. A listener is called only on a change.

## Not here

The wire (OpenPluginEditor, ClosePluginEditor, PluginEditorState), the UI banner
and the HostSupervisor binding are `plugins.host.editors.wire`: it needs a
protocol number from the coordinator and `engine/control` files held by
`interchange.stems`.
