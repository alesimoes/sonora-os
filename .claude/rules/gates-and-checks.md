# Gates and checks

- **Never disable, loosen or bypass a check to get green**: no sanitizer flag,
  test, ctest filter, label, gate step, timeout or `|| true` removed or added
  as a "fix". Why: 2026-11-05 a fix switched a sanitizer check off and turned
  the PR green without fixing anything.
- A slow check is made faster at its cause (smaller file, narrower target,
  better wait), never skipped. If you cannot, report it as unfixed.
- A check you did not run is "unverified" in the report, never "passing". Say
  the command and what it printed.
- The gate is the orchestrator's local CI (`tools/local-ci.sh`); CI-like
  claims by an agent are not the gate. Why: green by assertion cost days.
- A red check on a PR you did not touch is routed (see `.claude/PROCESS.md`
  failure table), not "fixed" in your PR.
