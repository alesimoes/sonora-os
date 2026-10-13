# ADR-0008 — Portability: platform code stays at the edges

- Status: Accepted by the owner 2026-10-12
- Date: 2026-10-12
- Decided by: the owner, in the coordinating session, 2026-10-12. The owner
  decisions are these:
  1. **"Keep portability from now on."** The code stays portable so that a
     build for another OS stays cheap. Building one is not decided.
  2. **A Windows release of the app may come later. If it does, its audio
     backend is ASIO**, the sound card's own driver. So the Windows audio API
     is chosen. Whether ASIO's SDK licence fits GPL-3.0 is still open (§6).
  3. **Linux first.** Phase 1 and Phase 2 on the appliance are the priority.
     Portability is a constraint on *how* we build. It is not a workstream
     that competes with the appliance, and no portability step may delay a
     Phase 1 or Phase 2 item.
  4. **The build can produce Sonora alone, without the OS.** Sonora here
     means the engine and the UIs. The OS is `iso/`, `audio-stack/`, the
     systemd units and live-build. The CMake structure selects the platform,
     so a Windows build of Sonora is a configuration, not a fork.

  The architect wrote the rule, the audit, the seams and the CI choice below.

## Context

The owner asked whether the recording software is part of the OS or could
also run on Windows. The answer:
- The engine and the UI are already separate programs, and the image only
  installs them (ADR-0007 §3).
- Qt is cross-platform (ADR-0006).
- The audio backend is a swappable contract (`contracts/backend`, ADR-0007
  §4.2, landing in M2). An ASIO backend would be one more driver.
- Nothing in the product *needs* Linux outside a few known places.

But nothing keeps it that way. Today:
- `engine/core` scans the session folder with `<dirent.h>` and creates it
  with `mkdir(2)`.
- Three test files in platform-neutral modules include POSIX headers.
- The root `CMakeLists.txt` passes GCC-style flags to every target, and it
  configures `drivers/jack` (pkg-config JACK, `REQUIRED`) unconditionally.

Every week of squads writing against Linux adds more of this. M7 is the worst
case: it is about to put Unix sockets, `memfd` and `SCM_RIGHTS` into
`engine/control` and `ui/client`. Portability added later is an audit of the
whole tree. Kept from now on, it is a list of a dozen lines, plus a rule that
a machine checks.

The appliance (Debian, RT kernel, PipeWire; ADR-0002, ADR-0003) remains the
product. This ADR changes none of it.

## Decision

### 1. The rule

**Platform-neutral modules use standard C++17 and the modules they may link
under ADR-0007 §1. Nothing else.** That means:
- no POSIX, Linux, JACK, PipeWire, ALSA, Win32 or macOS headers;
- no `#if`/`#ifdef` on a platform macro (`__linux__`, `_WIN32`, `__APPLE__`,
  `__unix__`, `_POSIX_*`);
- no OS paths in string literals (`/proc/`, `/sys/`, `/dev/`,
  `$XDG_RUNTIME_DIR`).

Platform code lives only in **edge modules**, and is reached through a small
seam whose header is itself neutral.

Compiler feature tests are not platform tests and stay allowed. One example is
`__has_cpp_attribute(clang::nonblocking)` behind `SONORA_RT` (ADR-0007, first
update). `ui/qt` may also use Qt, and only Qt. Qt is the portability layer
there (ADR-0006).

| Platform-neutral (standard C++17 only) | Edge: platform code allowed |
|---|---|
| `common/base` | `drivers/*`, each backend (`drivers/jack` now, `drivers/asio` later) |
| `common/audiofile` | `engine/app`, the process entry layer (§3) |
| `common/model` *(P2)* | `ui/tui`, the POSIX terminal front end |
| `contracts/*` | `common/platform/src/<os>/` (§3) |
| `engine/core`, `engine/control` | `drivers/surfaces` *(P5)*: MIDI I/O |
| `engine/plugin_client` *(P4)*, `engine/sync` *(P5)* | `plugins/host` *(P4)*: editor windows are display-server specific |
| `drivers/fake` | `<module>/tests/posix/`: OS fault-injection tests, built `if(UNIX)` |
| `ui/keymap`, `ui/client` | `tools/`, `iso/`, `audio-stack/`, `.github/` |
| `ui/qt` (plus Qt) | |
| `interchange/` *(P4)* | |

Two classifications need a reason:
- **`drivers/fake` is neutral.** It is the backend that lets every engine
  test run without hardware. It must therefore run wherever the neutral
  modules are tested.
- **Each edge module still keeps its code in POSIX or Linux files that a
  Windows build can leave out.** This rule moves platform code to the edges.
  It does not port anything.

**The real-time rules are unaffected.** CLAUDE.md and ADR-0007 §2 still
apply in full. A seam function that the audio thread can reach is declared
`noexcept SONORA_RT`, and the auditor audits it like any other RT callee. The
only one foreseen is the Phase 4 plugin wake. Portability never justifies a
lock, an allocation or a blocking call in the callback.

### 2. Audit of the tree at `e4f309a`

**Violations: 4 files on `main`, 1 more on an open branch.** For each one, the
fix is either a rewrite with the standard library or a move behind a seam.

| # | Where | What | Resolution |
|---|---|---|---|
| V1 | `engine/core/src/session.cpp:4-5`, `:16-20` (`make_dir`), `:110-115` (`next_free_take`) | `<dirent.h>`, `<sys/stat.h>`, `mkdir`, `stat`/`S_ISDIR`, `opendir`/`readdir`/`closedir` | **Rewrite with `std::filesystem`.** Use the `std::error_code` overloads only, so nothing throws. This runs on the control thread, never in the callback. The take-numbering behaviour that `test_takes` pins must not change. |
| V2 | `engine/control/tests/test_takes.cpp:10-11`, `:32` (`access`), `:56-58` (`opendir`/`readdir`), `:64-65` (`rmdir`) | POSIX in a neutral module's test | **Rewrite with `std::filesystem`.** |
| V3 | `common/audiofile/tests/test_wav_writer.cpp:4-5`, `:181-196` | `setrlimit(RLIMIT_FSIZE)` plus `SIGXFSZ` to simulate a full disk; checks `EFBIG` | **Move that one case** to `common/audiofile/tests/posix/test_wav_writer_full_disk.cpp`, registered `if(UNIX)`. The regression test is worth more than purity, and standard C++ has no way to fill a disk. The rest of `test_wav_writer` stays neutral. |
| V4 | `ui/keymap/tests/test_keymap.cpp:4-5`, termios helpers `:206-299`, `std::remove` on a directory at `:835`, `:899` | termios in `ui/keymap`'s test, and `remove()` of a directory, which works on POSIX and fails on the Windows CRT | **Part of M4**, which already rewrites `test_keymap` (ADR-0007, lessons from M1, item 1). The termios checks move to a `ui/tui` test. Directories are removed with `std::filesystem::remove`. |
| V5 | `ui/qt/src/measure.cpp:11`, `:19`, `:25`, `:33-34` on `feature/qt-shell` (65f54d1), not merged | `<unistd.h>`, `clock_gettime`, `sysconf(_SC_CLK_TCK)`, `/proc/self/task` | Monotonic time uses `std::chrono::steady_clock` (or `QElapsedTimer`). Process CPU uses `std::clock()`. **Per-thread CPU moves behind `common/platform`** (`thread_cpu_times()`). The POSIX implementation reads `/proc`. Other platforms return "unavailable", and the report prints that rather than zeros. Fixed on that branch before it merges, or in the first PR after. |

**Build-system violations** (not source, but they stop a neutral build):
- B1 — `CMakeLists.txt:12`: `add_compile_options(-Wall -Wextra)` applies to
  every compiler. It goes under a generator expression; MSVC gets
  `/W4 /permissive- /utf-8`.
- B2 — `CMakeLists.txt:33` adds `drivers/jack` unconditionally, and
  `drivers/jack/CMakeLists.txt` requires pkg-config JACK. Fixed by §3's
  platform selection.

**Already portable, so no change.** Each is recorded with the assumption that
the CI job of §4 will test:
- `common/audiofile/src/wav_writer.cpp:48` uses `fopen(path, "wbx")`.
  - Exclusive-create mode `x` is C11, and C++17 incorporates the C11 library
    by reference.
  - *Assumption:* MSVC's UCRT honours `x` and sets `errno = EEXIST`.
    `session.cpp:154` relies on that `errno`. ISO C does not require `fopen`
    to set `errno`; POSIX and the Microsoft CRT do.
  - The existing "take already on disk" case in `test_wav_writer` proves or
    disproves the assumption on the first run of §4's CI job.
  - If it fails there, exclusive create moves behind
    `common/platform` (`create_exclusive()`). It is not weakened.
  - *Update, M7 review (PR #38):* it moved there anyway, for close-on-exec
    (a take must not leak into the `sonora-tui` child). `create_exclusive()`
    is `O_CREAT|O_EXCL|O_CLOEXEC` on POSIX and `fopen(path, "wbx")` in the
    unsupported stub, so the assumption above still holds for Windows.
- `engine/core/src/disk_thread.cpp:19`, `:40` use `std::thread` and
  `sleep_for`. Standard.
- `common/base/tests/test_ringbuffer.cpp:6` uses `std::thread`. Standard.
- `session.cpp:23`, `:137` build paths with `/`. Windows accepts that.

**Platform code that is already in the right place.** These are the
Linux-only dependencies the owner asked about, each with its seam:

| Dependency | Where it lives | Seam that a Windows build goes through |
|---|---|---|
| JACK (`<jack/jack.h>`) | `drivers/jack` only | `contracts/backend`'s `AudioBackend` (M2). Windows gets `drivers/asio`, not created now. |
| Backend factory `make_backend()` | `engine/app/src/main.cpp:35` on `contract/backend` | It stays in the entry layer, because that layer knows which backends this platform compiled (§3). |
| `mlockall` | `engine/app/src/memory_lock.cpp:4-5`, `:35` on `contract/backend` (0e74180) | `engine/app`'s process-setup seam (§3). The Windows equivalent (`VirtualLock`, working-set limits) is not equivalent and is an open question (§6). |
| Quit signals (`SIGHUP`) | `engine/app/src/quit_signals.cpp:16-20` | Same seam. Windows would use console and service control handlers. |
| termios, `read(STDIN)`, `usleep` | `engine/app/src/main.cpp:15-16`, `:37-58`, `:172`, `:178`, `:222`, `:528`, `:552` | This is the terminal UI still living in `main.cpp`. M4 and M7 move it out to `ui/tui`. After M7, `main.cpp` holds only wiring. |
| termios in a public header | `ui/tui/include/sonora/tty_mode.h:7`, `ui/tui/src/tty_mode.cpp:4` | `ui/tui` is the POSIX front end. Its seam is `EngineControl`: any UI is a client. A Windows build simply leaves `ui/tui` out. |
| `$HOME` default session folder | `engine/app/src/main.cpp:293` | Process-setup seam (`default_session_root()`). |
| Unix socket, `memfd`, `SCM_RIGHTS`, `PROT_READ` (ADR-0007 §3) | Not written yet; M7 | `common/platform` IPC (§3). **This is the one that matters most**, because it would otherwise land in two neutral modules. |

### 3. The structure: the platform is selected, and seams are named

**The CMake platform selection.** One cache variable, `SONORA_PLATFORM`:

| Value | What configures | Used by |
|---|---|---|
| `linux` (default on Linux) | Everything we have today: neutral modules, `drivers/jack`, `drivers/fake`, `engine/app`, `ui/tui`, `ui/qt` when Qt is found | The appliance, `tools/build-engine.sh`, every existing CI job |
| `neutral` (default elsewhere) | Neutral modules and their tests only, plus `drivers/fake` and `common/platform` with its `unsupported` implementation | The portability CI job (§4), and a developer on any OS |
| `windows` | **Reserved.** Configuring with it today **fails at configure time** with "drivers/asio and the Windows entry layer do not exist yet; use SONORA_PLATFORM=neutral" | A future Windows ADR |

- Backends are a per-platform list: `SONORA_BACKENDS` defaults to `jack;fake`
  on `linux`, `fake` on `neutral`, and `asio;fake` on `windows` later. The
  entry layer links exactly that list, and `make_backend()` knows only what
  was linked.
- **Selection happens in CMake by choosing source files and directories,
  never by `#ifdef` in a neutral file.**
- **The OS is not in this build at all.** `iso/`, `audio-stack/`, the
  systemd units and live-build are driven by `tools/build-iso.sh`, and that
  consumes the `linux` build's `sonora-engine`. So "compile only Sonora"
  needs nothing new. It is the CMake root as it already stands (ADR-0007 M1),
  with the platform selected.
- Compiler-specific flags go under generator expressions
  (`$<$<CXX_COMPILER_ID:MSVC>:...>`). The neutral build sets
  `CMAKE_CXX_EXTENSIONS OFF`, so a GNU extension fails there and not later.

**Seam 1: `common/platform`** (target `sonora_platform`, owner rt-engineer).
- Neutral headers under `include/sonora/platform/`. Implementations live in
  `src/posix/` (the only real one now) and `src/unsupported/`.
- `src/unsupported/` returns a clear error such as "not available on this
  platform". Callers report it at start-up, so the failure is loud. It is
  what lets the neutral build link. It is **not** a Windows port.
- Layer 0, beside `common/base`. Any module may link it **except**
  `common/base` and `contracts/*`, which stay pure. This adds one module to
  ADR-0007 §1's graph. It is additive.
- **Created at the first PR that needs it, not before** (ADR-0007: an empty
  scaffold invites code written before the design). That is V5 if
  `feature/qt-shell` merges first, otherwise M7.
- It owns, as each need arrives:
  - **IPC (M7).** A `ControlChannel` (message-preserving, connect/accept,
    passes a shared-memory handle) and a `SharedRegion` (create
    read-write; map read-only in the client).
    - POSIX: `AF_UNIX`/`SOCK_SEQPACKET` under `$XDG_RUNTIME_DIR/sonora/`,
      `memfd`, `SCM_RIGHTS`, `PROT_READ`.
    - Windows later: a message-mode named pipe, plus a section object from
      `CreateFileMapping` with a handle duplicated read-only into the client
      (`FILE_MAP_READ`). That keeps ADR-0007 §3's guarantee: the kernel stops
      a client from writing engine state.
    - `EngineControl`, the wire codec and `EngineStateBlock` do not change.
      They are already transport-free.
  - **Diagnostics:** `thread_cpu_times()` (V5).
  - **Later, each with its own phase:**
    - file durability (`fsync`/`fdatasync`), for Phase 5 crash recovery;
    - direct I/O (`O_DIRECT`), a backlog item;
    - disk-thread priority or I/O class;
    - the Phase 4 plugin wake (futex or eventfd; `SONORA_RT`, auditor-gated).

  None of these may be written inline in `engine/core` or `engine/control`.

**Seam 2: the entry layer, `engine/app`** (owner rt-engineer).
- `engine/app` is the POSIX/Linux process entry: options, wiring, headless
  `--record`, and process setup.
- Its OS-specific part sits behind one neutral header,
  `engine/app/include/sonora/process_setup.h`:
  - `lock_memory()`, which becomes the `memory_lock` code from
    `contract/backend`;
  - `install_quit_handlers()`, which becomes `quit_signals`;
  - `default_session_root()`.
- Implementations: `engine/app/src/posix/`.
- A Windows entry later is `engine/app/src/win32/`, selected by
  `SONORA_PLATFORM`. A Windows service wrapper, if one is wanted, goes there
  too.
- `main.cpp` becomes neutral wiring once M4 and M7 have taken the terminal UI
  out of it. Until then it is still edge code, and that is allowed.
- These stay in `engine/app` rather than `common/platform`, because only the
  engine process needs them.

**Seam 3: the backend, `contracts/backend`** (M2, rt-engineer and
driver-engineer).
- Already decided by ADR-0007. This ADR adds one point: a future
  `drivers/asio` implements `AudioBackend` and changes nothing above it.
- ASIO's model has the driver own the callback thread, and buffer size and
  rate are set through the driver. That fits `open`/`activate`, `fatal()` and
  ADR-0003's "rate mismatch fails at start-up". This is unverified against the
  SDK (§6).

**The process split maps to Windows unchanged.**
- ADR-0007 §3's shape stays: a headless engine, the Qt UI as a client
  process, and a read-only state block.
- Only the transport underneath changes, and it is behind seam 1.
- systemd's role (ordering and restart) would be taken by a Windows service
  or a launcher. That is a question for the Windows ADR, not for the code.

### 4. Enforcement

**Two gates, because each one misses what the other catches.**

**A textual check.** This extends ADR-0007 M5's forbidden-include check, in
the same script and the same CI job.
- **Neutral modules get an allowlist, not a denylist.** The allowed includes
  are:
  - the C++17 standard headers, in their `<cxxx>` forms;
  - `"sonora/..."`;
  - for `ui/qt` only, `<Q...>` and `<Qt.../...>`;
  - any third-party header a later ADR admits for that module.
- On those same files, the check rejects:
  - `#if`/`#ifdef` on a platform macro;
  - the string literals `"/proc/`, `"/sys/` and `"/dev/`.
- Excluded from the check: `common/platform/src/<os>/`,
  `<module>/tests/posix/`, and every edge module.
- An allowlist catches the header nobody thought to deny: `<malloc.h>`,
  `<endian.h>`, `<execinfo.h>`. "Standard C++17" is a closed list, so this
  costs nothing extra.
- The owner's request said "denylist". The allowlist is stricter, and it
  satisfies the request.
- It runs in seconds on the Linux runner, so a violation fails before any
  compiler runs.

**A non-Linux compile, the semantic gate.** The textual check cannot see
code that compiles on Linux only because glibc headers leak a declaration
through a standard header. Examples: `::access` reached transitively, or
`ssize_t`. Only a real non-Linux toolchain catches those.

Options, with cost per run. The repository is public, so standard GitHub
runners cost nothing. The dollar figure is what the same minutes would cost
if it ever went private, at GitHub's current rates: Linux $0.006/min,
Windows $0.010/min, macOS $0.062/min. Today's Linux `engine` job takes 24 s
(run 38337928308). The durations below are estimates.

| Option | What it proves | Est. duration | Cost per run if private |
|---|---|---|---|
| **`windows-latest`, MSVC, `SONORA_PLATFORM=neutral`, build + `ctest`** | MSVC has no `<unistd.h>`, `<dirent.h>` or `<sys/stat.h>`, so any POSIX use fails to compile. The tests *run* against the real UCRT: `fopen "x"`, `errno`, `std::filesystem`, 8-byte lock-free atomics. It is also the toolchain an ASIO build would actually use. | 3–5 min (slow runner start, MSVC) | ~$0.03–0.05 |
| mingw-w64 cross-compile in a Debian container on `ubuntu-latest` | Compiles for Windows. **But mingw-w64 ships `unistd.h`, `dirent.h`, `sys/stat.h` and winpthreads**, so V1–V3 would compile there. Running the tests needs Wine (a large install, and not the real CRT). | 2–3 min, +2 with Wine | ~$0.01–0.03 |
| `macos-latest`, Apple Clang | Catches Linux-only APIs (`memfd`, `/proc`, `mlockall` flags). It is POSIX, so it accepts every V1–V4 violation. | ~3 min | ~$0.19 |

**Recommended: `windows-latest` with MSVC.**
- It is the only option that fails on exactly the violations found in §2.
- It runs the tests rather than only compiling them.
- It matches the owner's stated Windows target.
- It costs nothing while the repository is public, and about $0.04 a run if
  that changes.
- Its real cost is wall-clock: it will be the slowest job in CI. It runs in
  parallel with the others, so a PR waits for it but nothing else does.
- It is a **required check**. An optional portability job drifts red and
  stays red.
- `ui/qt` joins the job when it merges, with Qt from a cached
  `install-qt-action`, about 1–2 min warm (estimate). Until then,
  `ui/qt`'s neutrality is enforced by the textual check alone.

Mingw was rejected because it is cheaper only by checking less. macOS was
rejected because it costs 6x as much and passes every violation on `main`.

### 5. Migration

Small steps. Each one keeps `tools/build-engine.sh`, `ctest`,
`tools/test-engine-docker.sh` and the sanitizer jobs green. **None of them
blocks a Phase 1 or Phase 2 item.** When one would collide with appliance
work, it waits.

| Step | When, relative to ADR-0007 | What | Owner |
|---|---|---|---|
| P1 | After M2 (`contract/backend`) merges, because M2 also edits `session.cpp`; before M5 | V1: `session.cpp` to `std::filesystem`, no throwing overloads. `test_takes` must stay green unchanged. Add one case: the session path exists as a file, so `create_dirs` fails and `start_recording` refuses. | rt-engineer |
| P2 | With P1 or next to it; before M5 | V2 and V3: `test_takes` to `std::filesystem`; the full-disk case of `test_wav_writer` to `tests/posix/`, `if(UNIX)` | rt-engineer |
| P3 | After M2; any time before the Windows job | `engine/app` process-setup seam: `memory_lock` and `quit_signals` behind `process_setup.h`, under `src/posix/`; `default_session_root()` replaces `getenv("HOME")` | rt-engineer |
| P4 | Inside M4 | V4: `test_keymap` loses termios (those checks move to a `ui/tui` test) and removes directories with `std::filesystem` | ui-engineer with rt-engineer |
| P5 | Before `feature/qt-shell` merges, or the first PR after it | V5: `measure.cpp` on `std::chrono`/`std::clock`; creates `common/platform` with `thread_cpu_times()` (posix + unsupported) | ui-engineer (ui/qt), rt-engineer (common/platform) |
| P6 | With M5 (so after M2 and M4) | B1, B2: `SONORA_PLATFORM` and `SONORA_BACKENDS`, flags under generator expressions, `windows` fails at configure. The neutral allowlist, the platform-macro check and the OS-path check go into M5's script. | distro-engineer |
| P7 | Right after M5, before M6 | The `portable` CI job: `windows-latest`, MSVC, `SONORA_PLATFORM=neutral`, build + `ctest`, made a required check. Record the first 10 run durations in `brain/research/`. | distro-engineer; rt-engineer and ui-engineer fix what it finds in their modules |
| P8 | Inside M6 | The §1 table goes into `.claude/OPERATIONS.md` and the charters. The rule is proposed for CLAUDE.md's conventions, and that edit is the owner's. | distro-engineer drafts; owner approves CLAUDE.md |
| P9 | Inside M7 | IPC behind `common/platform` (`ControlChannel`, `SharedRegion`; posix + unsupported). `engine/control` and `ui/client` contain no socket or mapping code. | rt-engineer, ui-engineer |
| P10 | When `ui/qt` is on `main` | Qt added to the `portable` job | distro-engineer |

## Rationale

- **Cheap now, a rewrite later.** Today there are 4 files to fix on `main`
  and 1 on a branch, all small. After M7 and Phase 2 without the rule,
  sockets, mappings, durability and direct I/O would all sit inline in
  `engine/core` and `engine/control`. Porting would then mean auditing the RT
  module line by line, which is the expensive kind of change.
- **The seams already exist for other reasons.** The backend contract
  (M2), `EngineControl` (M4) and the process split (§3 of ADR-0007) were
  decided for squads and for crash isolation. This ADR only insists that the
  OS stays behind them and does not leak around them.
- **A machine checks it.** Like ADR-0007's dependency graph, a portability
  rule that only reviewers enforce erodes within a month of parallel squads.
  The Windows job makes a violation red within minutes.
- **It is a constraint, not a workstream (owner decision 3).** P1–P10 are
  S-sized, and most ride inside migration steps already planned.
- **It does not touch the appliance.** The Linux build is configured as
  today. The RT rules, the RT kernel and PipeWire are untouched. Losing a
  take is still the worst outcome, and nothing here adds code to the
  callback.
- **`std::filesystem` over a seam for session folders.** It is standard, it
  has non-throwing overloads, and it is already the right tool on Linux.
  A seam would be code that we own and that does what the library already
  does.

## Consequences

- One more required CI job, and probably the slowest one (3–5 min,
  estimated). There is no dollar cost while the repository is public.
- A neutral module that wants an OS facility has to name it in
  `common/platform` or in `engine/app`'s process-setup header. That is a
  little friction on purpose: it is where an "it only works on Linux"
  decision becomes visible.
- `common/platform`'s `unsupported` implementation means a neutral build
  *links* but cannot *run* the IPC or the diagnostics. Anything that depends
  on them fails at start-up with a message, never silently.
- `ui/tui` stays POSIX-only. A Windows build of Sonora has the Qt UI only,
  until someone decides on a Windows console front end.
- Paths are `std::string` in neutral code. On Linux they are UTF-8 bytes. On
  Windows a narrow `fopen` uses the ANSI code page unless the application
  manifest opts into UTF-8. That is left to the Windows ADR (§6), and the
  neutral code does not need to change for either answer.
- Reviewers gain one question per diff in a neutral module: "would MSVC
  accept this?" The CI job answers it, so they do not have to.

## 6. Not decided here, and open questions for a future Windows ADR

**Not decided:**
- shipping a Windows app;
- when a Windows app would ship;
- whether a Windows build is ever produced outside CI.

`SONORA_PLATFORM=windows` stays a configure-time error until that ADR exists.

**Decided by the owner, not reopened:** the Windows audio backend is ASIO.

**Open, each to settle before code depends on it:**

1. **ASIO SDK licence against GPL-3.0-or-later.**
   - What is known: historically the ASIO SDK was available only under
     Steinberg's proprietary licence agreement, which was generally
     considered incompatible with distributing GPL software that links it.
   - Press coverage and a Steinberg press release dated 2025-10-15 report
     that Steinberg moved ASIO to a dual licence, adding GPLv3 beside the
     proprietary one.
   - **That is reported, not verified here.** Before any ASIO code lands, the
     licence text in Steinberg's current SDK distribution must be read and
     recorded in `brain/research/`. The researcher does this; the architect
     does not guess. The record must answer:
     - whether the GPLv3 option is "v3 only" or "or later";
     - what a GPL-3.0-only component does to our GPL-3.0-or-later binary
       (the combined binary would be distributable under GPL-3.0 only);
     - whether the trademark terms allow the name "ASIO" in the UI and the
       documentation.
   - Until then `drivers/asio` is not created, and no ASIO header enters the
     repository.
2. **WASAPI exclusive as the licence-free fallback.** If the ASIO licence
   turns out unusable, WASAPI in exclusive, event-driven mode is the
   Windows-native path with no SDK licence. It does not change the owner's
   choice of ASIO. It is recorded so that a licence answer does not block
   the port. Its latency on the owner's interface is unmeasured.
3. **Real-time scheduling.**
   - Linux uses an RT kernel with `SCHED_FIFO` under `rtprio 95`.
   - Windows has no RT kernel. Its audio threads run at elevated priority
     through MMCSS (`AvSetMmThreadCharacteristics("Pro Audio")`), and with
     ASIO the driver owns the callback thread.
   - The same rules hold: no allocation, no locks, no blocking. What changes
     is the guarantee. ADR-0002's "the image we test is the image that ships"
     does not exist on a user's Windows install, and DPC latency from
     third-party drivers is outside our control.
   - The Windows ADR has to say what zero-xrun claim, if any, we make there.
   - Memory locking (`VirtualLock`, bounded by the working-set size) is not
     equivalent to `mlockall(MCL_FUTURE)`. It must be measured before we
     claim an equivalent.
4. **The M7 process split on Windows.**
   - Message-mode named pipes stand in for `SOCK_SEQPACKET`.
   - A section object with a handle duplicated read-only stands in for
     `memfd` + `SCM_RIGHTS` + `PROT_READ`. The kernel should enforce
     read-only through the handle's access mask. That is unverified, and
     it needs a test that tries to write and expects a fault.
   - 8-byte atomics in a mapping shared across processes are expected to be
     lock-free and address-free under MSVC on x64, as they are under GCC.
     The `static_assert`s of ADR-0007 check that at compile time on the
     `portable` job.
   - Process supervision (systemd's `Restart=always`) needs a Windows
     equivalent: a service, or a launcher.
5. **Path encoding.** UTF-8 manifest (`activeCodePage`) or
   `std::filesystem::path` at the file-opening boundary.

## Unverified

- **The CI durations** in §4 are estimates. P7 records the first 10 runs.
  If the median is above 10 min, revisit: caching, or splitting the build
  from the tests.
- **`fopen "wbx"` and `errno == EEXIST` under the UCRT** are expected, not
  checked. The first `portable` run settles it.
- **ASIO's fit to `AudioBackend`**: driver-owned buffer size and rate,
  reset requests. This is argued from the API's general shape, not from the
  SDK. It goes to the researcher together with the licence item, and only
  when the owner opens a Windows ADR.
- **Read-only enforcement of a duplicated section handle** (§6, item 4).

## Alternatives rejected

**Do nothing until a Windows build is decided.** It costs nothing now. It
costs an audit of every module later, at its most expensive point: after M7
has put sockets and mappings inline, and after Phase 5 has put `fsync` and
direct I/O inline in the RT module's neighbours. The owner chose to keep the
option cheap, and this alternative is the one that makes it expensive.

**Portability through `#ifdef` inside modules.** No new seam is needed, and
each file can carry both versions. Rejected because:
- every neutral file becomes two programs, and the one CI does not build
  rots;
- the RT auditor would have to read both branches of every `#ifdef` in
  `engine/core`.

Selecting source files in CMake keeps each file one program.

**A portability library: Boost, APR, or Qt Core in the engine.** It would
solve IPC, filesystem and threads in one go. Rejected because:
- it puts a large third-party body into the engine process, the one that
  holds the take;
- `std::filesystem` and `std::thread` already cover what neutral code needs;
- Qt in the engine reverses ADR-0007 ("nothing outside `ui/qt` includes
  Qt").

The seams we need are a handful of functions.

**Build the Windows port now.** It would prove the rule by shipping.
Rejected by the owner: Linux first. It would compete with Phase 1 for the
same squads, and the hardware the Phase 1 exit criteria need is still not
attached.

**A denylist of platform headers, as first requested.** It would match
ADR-0007 M5's existing check. Kept for the edge modules' forbidden edges
(JACK outside `drivers/jack`, Qt outside `ui/qt`). For neutral modules the
allowlist is stricter and no more expensive (§4).

**mingw cross-compile or macOS as the non-Linux job.** Both are covered in
§4: mingw is cheaper by checking less, and macOS is dearer and passes every
violation on `main`.
