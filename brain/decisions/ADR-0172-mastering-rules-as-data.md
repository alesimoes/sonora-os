# ADR-0172: The mastering rule table is data, loaded at start-up

Status: accepted (2026-11-14), recorded after the code merged in #771

## Summary

1. **The numbers the suggestion pass uses live in a file, not in the binary.**
   The mastering rule table (target curve, band layout, balance groups,
   dynamics, stereo and loudness constants) is
   `plugins/analyze/data/mastering_rules.json`. `sonora-analyze` reads it once,
   at start-up, before it touches any audio.
2. **No built-in copy.** If the file is missing or malformed, the CLI prints a
   message and exits 1. It never falls back to numbers compiled into the
   binary, so the file and the binary cannot silently disagree.
3. **The output does not change.** The golden report
   `plugins/analyze/tests/golden/mastering_suggestions.txt` was captured from
   the compiled-in table before the move, and the report produced from the file
   must be byte-equal to it.

## Context

The suggestion pass (ADR-0046 §2) turns a whole-mix analysis into settings,
each with a title and a reason. Its thresholds and targets were compiled into
`plugins/analyze/src/mastering_rules.cpp`. Tuning the target curve or a
threshold therefore meant a code change, a rebuild and a review of C++ for a
change that is only a number. The owner and the mastering track want the
numbers adjustable without touching the code path that the reports and tests
depend on.

The rule table has no real-time role. It is read by the offline `mastering`
run, which already runs off every audio thread (ADR-0046 §4), so reading a
file at start-up does not reach the `process` path (CLAUDE.md "Rules for the
real-time path" does not apply).

## Decision

- The rule table is a JSON document, `version: 1`, with: `target_db` (ten
  octave bands, 31.5 Hz to 16 kHz), `band_names`, `anchor_bands`,
  `sub_bass_margin_db`, `balance_groups` (rule id, bands, wording, gain and
  frequency keys), `dynamics`, `stereo` and `loudness`.
- `load_mastering_rules(path, rules, error)` parses the file into
  `MasteringRules` and returns false with a message on a missing key, a wrong
  type, a wrong entry count (ten bands) or a band index out of range. It is the
  only way the table is built.
- `sonora-analyze` finds the file in this order:
  1. `SONORA_ANALYZE_RULES`, if set and non-empty. An explicit path is used as
     given and is not searched for elsewhere.
  2. `data/mastering_rules.json` next to the binary (the build tree).
  3. `../share/sonora/analyze/mastering_rules.json` (the installed layout).
  If none exists, the error names every path tried and the exit code is 1.
- The build copies the file beside the binary (`plugins/analyze/CMakeLists.txt`),
  and the tests read it through `RULES_PATH`.
- A change to a value in the file is a rule change. It is committed with the
  golden report before and after, as CLAUDE.md requires for any golden
  regeneration. A change to the report's wording with unchanged numbers is a
  change to the golden file too; the byte-equal test is what catches it.

## Rationale

- **A number is data.** Target curves and thresholds are tuned by ear and by
  measurement. Keeping them in a file lets that tuning be reviewed as a diff of
  numbers, without a C++ review or a rebuild of the analysis code.
- **No fallback means one source of truth.** A built-in copy would keep
  working when the file was broken or deleted, and the report would quietly
  use stale numbers. Refusing to run is the honest failure for an offline
  tool whose whole output is those numbers.
- **The golden report is the proof of the move.** Capturing the golden file
  from the compiled-in table before the change, and requiring the file-driven
  report to be byte-equal, shows the move changed where the numbers come from
  and nothing else.
- **JSON, as the rest of the tree.** `common/json` already parses the manifest
  and project files; a second format (YAML, TOML) would add a parser to the
  tree for no gain.

## Consequences

- Editing a rule is a data change, but it still changes the sound of the
  suggestions, so it needs the same golden discipline as code. Reviewers check
  the golden diff, not just the JSON.
- `sonora-analyze` cannot run without `mastering_rules.json`. Packaging must
  install it (the `../share/sonora/analyze/` path), or the CLI fails at start-up
  on the installed system. This is intended, and it is the failure the owner
  should see first.
- The table is read once per process. Changing the file while `mastering` runs
  has no effect on that run; a new run picks it up.
- The lookup of the executable's own path reads `/proc/self/exe`, which exists
  on Linux only. The CLI is an application, not a neutral module (ADR-0008),
  but a Windows build of the analysis tool needs `SONORA_ANALYZE_RULES` or a
  port of that one lookup. Recorded as a follow-up, not decided here.
- The schema is `version: 1`. The loader refuses any other `version` with a
  message, rather than guessing; an absent `version` is read as 1. A later
  incompatible change bumps the version.

## Alternatives rejected

- **Keep the table compiled in.** Simplest, and the golden test would still
  pass, but every tuning of a number remains a code change and a rebuild, and
  the owner's stated aim (numbers adjustable without touching the code) is
  not met.
- **Compiled-in table as a fallback when the file is missing.** Keeps the tool
  running, but the report then comes from numbers nobody can see, and a broken
  deploy looks like a healthy one. Rejected: failing loudly is the point.
- **Load the table at every `mastering` run from a path given on each call.**
  Makes the tool stateless about rules, but every caller must know where the
  table lives. The start-up search with one env override covers the build
  tree, the installed layout and tests without that burden.
- **YAML or TOML.** Nicer to hand-edit, but no parser in the tree; JSON is what
  `common/json` already reads. Rejected for the parser cost.
- **Hot reload (watch the file).** Not needed for an offline tool and it would
  add file watching to a process that otherwise has none. Rejected.

## Unverified

- The Windows path for the analysis CLI is not built or run here; the
  `/proc/self/exe` lookup is Linux-only, as noted above.
- The installed-layout path (`../share/sonora/analyze/`) is not exercised by a
  packaged image; it is covered only by reading the code.
