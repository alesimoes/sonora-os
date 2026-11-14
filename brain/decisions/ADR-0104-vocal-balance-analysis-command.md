# ADR-0104 - Vocal Balance: the `sonora-analyze vocal-balance` command

- Status: Accepted
- Date: 2026-11-08
- Item: `native.vocal_balance`
- Relates to: ADR-0011 (§4.1 AI produces settings, N5/N6), ADR-0046 (exit
  codes), ADR-0066 (§5 subcommand fragments), ADR-0038 (Plan)

## Context

The processor (Sonora Vocal Balance, a dynamic resonance suppressor), its
golden files and the offline half (`analyse_vocal`, `vocal_balance_params`,
`VocalBalancePlanSource`) are on main (#436). What is missing is the way an
operator runs the whole-track analysis: nothing calls `analyse_vocal` from
outside a test. The analysis allocates and takes seconds on a song, so it
cannot live in the plugin's `process` path (ADR-0011 §1.4).

## Decision

1. `sonora-analyze vocal-balance <vocal.wav> [--target-level-db N]
   [--out report.json]` reads a mono or stereo WAV (stereo is summed to
   mono), calls `analyse_vocal`, and writes a JSON report: the measurements
   (resonances, zones, sibilance, level) and the plan steps with their
   one-sentence reasons. Audio is never written. Without `--out` the report
   goes to stdout. `--target-level-db` defaults to -20 and feeds the Trim step.
2. Exit codes follow ADR-0046: 0 report written; 2 usage (missing or unknown
   argument; the line `usage: sonora-analyze vocal-balance ...` is on
   stderr); 1 analysis failure (file unreadable, too short, too quiet,
   non-finite) with the reason on stderr; 130 cancelled. A failure never
   leaves an empty file or an empty success.
3. Rule-based only, no model (ADR-0011 §4.1). Model-assisted analysis is N6.
4. No new processor and no change to the Processor, its parameters or the
   audio path. The editor is the generic host editor (`plugins.host.editors`);
   a bespoke faceplate is out of scope.
5. The command follows the ADR-0066 fragment pattern: `app/cmd_vocal.cpp`
   replaces the stub `app/cmd_vocal_stub.cpp` by existing; `main.cpp` and
   `commands.h` already carry the table entry and the declaration
   `run_vocal_balance_command`. The stub answers a missing file with the usage
   line (exit 2) so the `analyze_cli_vocal_usage` test holds before and after.

## Rationale

`sonora-analyze` is the existing home of offline, non-real-time passes with a
JSON report and the ADR-0046 exit codes; engine/control already knows how to
start it. The report is settings and reasons, applied as undoable session
operations the operator sees first (ADR-0011 §4.4).

## Consequences

- One more table entry and one more file pair; no change to other passes.
- The report format becomes a contract for the session layer's plan builder.
- Unverified: the thresholds were set on synthetic vocals only. Real-voice
  tuning needs a researcher-cleared corpus (ADR-0011 §4.6). Measurement: run
  the command on several unprocessed vocal stems and record the zone and
  resonance counts.

## Alternatives rejected

- **A new binary.** A second process to ship, sign and supervise for one pass;
  `sonora-analyze` already has the dispatch, signals and exit codes.
- **Analysis inside the plugin.** It allocates, holds strings and takes
  seconds: it breaks the real-time rule.
- **Model-based analysis now.** Needs a cleared corpus and ONNX; the
  rule-based pass is explainable per number. Left to N6.
