# ADR-0157 - test_takes runs on std::filesystem and the standard library clock

- Status: Accepted
- Date: 2026-11-12
- Item: `architecture.portability.tests_fs`
- Relates to: ADR-0008 (portable neutral code, section 1), ADR-0029 (take files)
- Wire: no protocol change, no format change, no production code change.

## Context

`engine/control/tests/test_takes.cpp` is a neutral test, but it reached for
POSIX directly: `<dirent.h>` and `opendir`/`readdir`/`rmdir` to wipe a
session directory, `<unistd.h>` for `access` and `usleep`. Those headers do
not exist on the MSVC leg of the portable build, so the file cannot compile
there, and ADR-0008 section 1 says neutral code takes no platform header.

## Decision

1. **Directory wipe is `std::filesystem::remove_all`.** `wipe()` calls the
   non-throwing overload with an `std::error_code` and ignores the code: an
   absent directory is the state the test wants, and a leftover that cannot be
   removed still fails the test's own checks.
2. **Existence is `std::filesystem::exists`** with an error code, not
   `access(F_OK)`.
3. **The disk-thread wait keeps its bound and uses the standard library
   sleep.** The loop still waits on observable state (`take_failure()`, at most
   200 polls of 10 ms) and does not depend on the sleep length; `usleep` is
   replaced by `std::this_thread::sleep_for`.
4. **The full-disk WAV test stays where it is.** `common/audiofile/tests/posix`
   (built under `if(UNIX)`) already covers the POSIX write path; this file keeps
   only the engine-level take behaviour.

## Consequences

- `test_takes.cpp` compiles with no platform header, so it no longer blocks a
  Windows build of the engine's tests. It is not in `sonora_portable` today
  (that target collects only `common/` and `plugins/native/`); adding it is a
  separate change.
- `wipe()` now removes everything under the test's directory, not only
  `Audio Files/`. Each test owns its directory name, so nothing else is lost.

## Alternatives rejected

- **Keep `<dirent.h>` behind `#if !defined(_WIN32)`.** That is a platform
  branch in a neutral test; the standard library already does the job.
- **A fixed `sleep` longer than the poll.** Wall-clock waits are what
  `testing-and-flakes.md` forbids; the bounded poll on state stays.
