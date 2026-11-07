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

## The plugin package

The plugins are a product of their own (owner, 2026-11-05), so their build
does not need the engine, the host or the UI. `-DSONORA_PLUGINS_ONLY=ON`
configures only `common/*` and `plugins/native`.

    tools/package-plugins.sh          # Linux: LV2 + VST3, in the sonora-plugins container
    tools/package-plugins.ps1         # Windows: VST3 with MSVC (VS 2022 Build Tools)
    tools/build-plugins.sh --plugins-only   # just the build and the native tests (ctest -L native)

The archives land in `dist/` (gitignored):
`sonora-plugins-<version>-linux-x86_64.tar.gz` and
`sonora-plugins-<version>-windows-x64.zip`, each with the bundles, `licenses/`
(Sonora GPL-3.0, DPF's ISC text, a NOTICE; see
`brain/research/plugin-licences-onnx.md`) and a README. The version is
`SONORA_VERSION`, else `git describe` (a tree with no tag reads
`0.0.0-<commits>-g<sha>`). Only what the install rules of
`plugins/native/format` install is packaged, so a processor marked
`EXPERIMENTAL` (ADR-0011 3.5) is not.

`tools/local-ci.sh` runs the same build, tests and archive as its `plugins`
step, in a scratch folder; `--release` keeps both archives in `dist/`. The
Windows zip is built with `tools/build-portable.ps1 -Package` (the `windows`
step). The Windows build has no LV2 and no bundle driver or validator (both
are POSIX); `package-plugins.ps1` checks the module's `GetPluginFactory`,
`InitDll` and `ExitDll` exports instead. Not yet done: code signing, and
loading the Windows bundles in a Windows host.
