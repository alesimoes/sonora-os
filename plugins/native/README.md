# plugins/native

Sonora's own processors (ADR-0011). The directories are found by CMake, not
listed: `plugins/native/CMakeLists.txt` globs `*/CMakeLists.txt` (with
`CONFIGURE_DEPENDS`, alphabetical) apart from `sdk`, `format` and `measure`,
which come first.

## Adding a processor `<name>`

Create `plugins/native/<name>/` and touch nothing outside it:

1. `CMakeLists.txt`: the library `sonora_native_<name>`, the suite library
   `sonora_native_<name>_suite` (`tests/suite.cpp`), `sonora_native_quality_test(<name>)`,
   and `sonora_native_plugin(<name> ...)` for the bundles. Copy
   `saturator/CMakeLists.txt`.
2. `tests/suite.cpp` defining `sonora::native::suites::<name>(const measure::Args&)`
   and `<name>_golden_config()`.
3. `tests/host_fixture.inc`: the one `{"<name>", ...},` entry of the host
   fixture's `Target` table (see `saturator/tests/host_fixture.inc`).
4. `include/sonora/native/<name>.h`, `quality.json`, `tests/golden/`.

What derives from the directory, with no edit:
- the root `add_subdirectory` (this directory's CMakeLists.txt);
- the `sonora-dsp-measure` link line, `--processor <name>` dispatch and
  host-fixture table (generated into `build/plugins/native/generated/`);
- `tools/dsp-cpu.sh`'s default processor list (`*/tests/suite.cpp`);
- the Portable (MSVC) job's targets: it builds `sonora_portable`, every
  library and executable under `common/json`, `common/dsp` and
  `plugins/native`, so `add_executable(test_...)` anywhere there is covered.

A directory with a `CMakeLists.txt` but no `tests/suite.cpp` (`ampcab`, for
now) is built and tested but not driven by the harness.

## Processors with a research note

- `multiband`: 3 or 4 band compressor, Linkwitz-Riley or linear-phase
  crossovers (`common/dsp/include/sonora/dsp/crossover.h`); design in
  `brain/research/native-multiband-n2.md`.
