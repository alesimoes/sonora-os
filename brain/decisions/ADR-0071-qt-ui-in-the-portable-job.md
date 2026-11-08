# ADR-0071: The Qt interface in the nightly Portable job, from a cached Qt install

- Status: Proposed (backlog P10 of ADR-0008 section 8, architecture.portability.ci_qt_windows)
- Builds on: ADR-0008 (portability, P10), ADR-0061 (nightly, not per PR), ADR-0006 (Qt, dynamic)

## Context

ADR-0008 section 1 calls `ui/qt` neutral-plus-Qt: it may include Qt and standard C++17, nothing POSIX. Until now only a textual check enforced that. The `portable` job in `nightly.yml` (windows-latest, MSVC) built only common/json, common/dsp, common/platform, common/audiofile and plugins/native. The root `CMakeLists.txt` returned under `SONORA_PLATFORM=neutral` before reaching `ui/qt`, and `ui/qt` skips itself when Qt 6.7+ is missing, so MSVC never compiled the interface.

The Linux image (`tools/Dockerfile.ui`) pins no Qt version: it installs Debian trixie's `qt6-base-dev`, which is 6.8.2 (the `nightly.yml` `ui` job and `ui/qt/CMakeLists.txt` say "6.8"; the patch number is from the trixie package, not read from a built image).

## Decision

1. **Scope.** The `portable` job also configures and builds the `ui/qt` targets and what they link (`contracts/api` with its fake, `ui/keymap`, `ui/client`, `common/model`, `common/midi`, `common/session_files`), and runs the tests labelled `ui`. `ui/tui` and anything POSIX stay out. A root option `SONORA_PORTABLE_UI` (default OFF) lets `SONORA_PLATFORM=neutral` reach them; it adds the target `sonora_portable_ui`, which collects the executables and static libraries of `ui/qt`. The existing steps and their order are unchanged; the new steps are appended in a second build directory (`build-ui`).
2. **Qt.** `jurplel/install-qt-action` (aqt), pinned by full commit SHA `b344308e4483fc892e0d27b3cd1b173ebdd95f06` (v4.5.0), Qt `6.8.2`, `win64_msvc2022_64`, `cache: true` so the download is not repeated. Tests run with `QT_QPA_PLATFORM=offscreen`.
3. **When.** Nightly only (ADR-0061); never in `ci.yml`, never per PR. `timeout-minutes` of the job rises from 25 to 60 for the Qt install on a cold cache, a second configure and the MSVC build of ui/qt.
4. **Red is a finding.** A failure is an ADR-0008 finding: platform code moves to an edge module; the target is never filtered out. Excluded for a reason that is not a portability defect: tests that run `sonora-engine` as a process (`client_socket_engine`, `ui_replica_reconnect`) are already registered only `if(UNIX AND TARGET sonora-engine)`; the neutral build has no engine, so they are not registered. The `ui_render` pixel test, if it needs a display that `offscreen` does not give, is listed here when the first run shows it.

## Rationale

The one place a Windows compiler is available is this job; a Qt module that includes `<unistd.h>` is exactly what ADR-0008 wants caught. aqt with a cache costs a download once; the Qt version equals the Linux one so a difference is not Qt's. Pinning by SHA keeps the nightly reproducible and a moved tag harmless.

## Consequences

- The nightly Portable job is longer. A new ui/qt target is compiled under MSVC with no edit to the workflow (the target collects them).
- The root `CMakeLists.txt` gains an option and a block before the neutral early return; `common/model`, `common/midi`, `common/session_files` and `contracts/api` are configured under this option (their tests are labelled by module as on Linux but only `ui` runs).
- `ui/client` (sockets through `common/platform`) is built too; if it is not MSVC-clean, that is the first finding.

## Alternatives rejected

- Qt from `choco`/`vcpkg`: slow source builds or unpinned versions.
- Filtering failing ui targets out of the job: hides the defect the job exists to find.
- Adding it to `ci.yml` per PR: ADR-0061 keeps Windows out of the PR gate.
- A system Qt on the runner: the version drifts with the image.

## Unverified

No Windows runner was available to the author; nothing here has been configured, compiled or run under MSVC. The first nightly run is the proof. Specifically unverified: that Qt 6.8.2 is the patch version of the trixie image; that aqt's base install provides Qml, Quick and Test for `find_package`; that `common/model`, `common/midi`, `ui/keymap`, `contracts/api` and `ui/client` compile under MSVC; that the `ui` tests pass offscreen. The P10 line stays open until that run is green and its duration recorded.
