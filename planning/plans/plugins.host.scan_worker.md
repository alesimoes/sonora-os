# Plan: plugins.host.scan_worker - close-out

Status: closed (docs-only close-out; no slice left to build)
Written by: engineer (eng-11)   Dispatched by: lead   Architect sign-off: N/A - close-out note, no design or contract work remains
Decision of record: ADR-0144 (.vstpreset Info names, and what the scan-worker item leaves open). ADR number 220 is reserved and not used.

## 1. Goal and user experience
N/A - the item's user-facing work is done. The scan worker, catalog and probe record are on main.

## 2. Design
N/A - no screens or faceplates in this close-out.

## 3. Contracts and integrations
N/A - no contract, protocol or format change (probe record stays version 3, `kProtocolVersion` unchanged; ADR-0144).

## 4. Engine / host implementation
Done on main:
- PR #95 (c1077a4b): ADR-0012 checklist 5, the plugin scan worker and catalog.
- PR #635 (1499b7bf, titled [partial]): a `.vstpreset` is listed by its Info chunk's Name (ADR-0144).
- PR #722 (81d622d9): the server owns FactoryPresetReader; plugin.rescan forgets blocked presets.
- BUILD_ID hook: on main (7b8603f4 stamps BUILD_ID; b97a6fdf adds the smoketest BUILD_ID check).

## 5. UI
N/A - no UI change in this close-out.

## 6. Tests
N/A - no code change. The existing tests for the above landed with their PRs.

## 7. Docs
This note is the only change. ADR-0144 already decides the open lines of the item.

## 8. Open items (not in this close-out)
- Stick-hardware run: owner-parked. It needs the physical USB stick and is not an agent task.
- Follow-ups: none recorded here; any new line goes to the BACKLOG through the coordinator's chore PR.

## Acceptance
- `git diff --name-only origin/feat/plugins.host.scan_worker` lists only `planning/` paths.
- No source, test, build or ADR file changed.
