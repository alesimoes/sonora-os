# ADR-0163 — Loop catalog interfaces

Status: accepted.

## Summary

- New neutral module `common/loops` (header-only, no dependencies) holds the
  contract for the loop browser: `LoopEntry`, `LoopQuery`, `ILoopSource`,
  `ILoopAudition`, `ILoopInserter`, plus fakes for tests.
- Everything here runs off the audio thread; nothing enters
  `Session::process()`.
- No protocol change, no file-format change. `kProtocolVersion` untouched.

## Decision

- `ILoopSource` lists and searches entries. Empty query fields do not filter;
  text matches name or tags case-insensitively; BPM filters by a fractional
  tolerance and an entry with unknown BPM (0) never matches a BPM filter.
- `ILoopAudition` previews an entry at the session tempo; `start` returns
  false if it cannot play.
- `ILoopInserter` places an entry on a track at a frame position through the
  normal edit operations; it returns false on refusal.
- Real implementations (folder scanner, preview, inserter) follow in later
  slices of `logic.loop_browser`, and depend on this module, not the reverse.
