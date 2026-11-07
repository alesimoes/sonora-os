# Testing and flakes

- Tests are by target: a PR builds the CMake targets its files reach and
  runs their tests (`tools/micro-build.sh`, ADR-0061); main runs everything
  nightly (`.github/workflows/nightly.yml`).
- Every test carries its module's ctest label (ADR-0007 section 7); a new
  test target is wired into the label function in the root `CMakeLists.txt`.
- No test depends on wall-clock time. Wait on an event or observable state
  with a progress-bounded wait; a longer timeout or a sleep is not a fix.
  Why: timing tests failed unrelated PRs for 3 days (2026-11-04/06).
- Tests that need real timing carry the `timing` label and run serial
  (`RUN_SERIAL`); the 12 long DSP suites carry `slow`. Do not add a label to
  hide a flake.
- A flake is reproduced (repeat, load, TSan) before and after the fix, and
  both rates are in the report (`flake-fixer`, skill `fix-flaky-test`).
- Plugins and DSP: run with `SONORA_DSP_QUICK=1`; a new suite stays under
  about 60 s in Release; a new target also goes in the Portable (MSVC) job
  `--target` list in `.github/workflows/ci.yml`.
- Every RT bug fix gets a regression test written first.
