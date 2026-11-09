# ADR-0117 - Smart Controls: a macro fans out to insert parameters

- Status: Accepted
- Date: 2026-11-08
- Item: `logic.smart_controls`

## Context

The Logic workflow puts up to eight macro knobs per track, each driving
several plug-in parameters. We reproduce the behaviour, not any file format.
The engine already has a parameter gesture (begin, PluginParameterChange,
one SetInsertParams at the end). Nothing new may reach contracts, the model,
the protocol or the session format.

## Decision

1. `ui/smart_controls` is a Qt-free module (pure C++17, links `common/json`
   only). It holds `SmartControlSet` (at most 8 `Macro`, unique names), each
   macro a list of `Mapping {insert, key, lo, hi, invert}`.
2. A macro value m in [0,1] sets each mapped parameter to
   `lo + (invert ? 1-m : m) * (hi - lo)`. `lo > hi` is allowed (it reverses).
3. `apply()` talks to a `ParamSink` (begin/set/end per insert, plus
   `has_param`). Mappings are grouped by insert: one begin/set.../end per
   touched insert. A mapping whose insert or key the sink does not know is
   kept in the set and skipped when applying; it is counted, never dropped.
4. Validation returns a reason string: more than 8 macros, empty or duplicate
   macro name, duplicate (insert,key) inside a macro, non-finite lo/hi are
   refused; `apply` refuses a value outside [0,1].
5. Persistence is a `SmartControlsStore` interface; the JSON store writes
   schema 1. A corrupt file or another schema version is reported with a
   reason and the in-memory set is left untouched.
6. The Qt panel (a later task) is the only consumer; it hooks into the build
   from inside `ui/smart_controls`, so no file outside the module is edited
   beyond one guarded `add_subdirectory` in `ui/client/CMakeLists.txt`.

## Consequences

Presets move between sessions as plain JSON. The engine sees ordinary
parameter gestures, so undo and the audio thread are unaffected.
