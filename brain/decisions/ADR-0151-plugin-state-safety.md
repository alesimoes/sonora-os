# ADR-0151 - Plugin state safety: one save at a time, verified files, a visible fault

- Status: Accepted
- Date: 2026-11-09
- Item: `plugins.host.state`
- Relates to: ADR-0029 (plugin presets and state files, section 6), ADR-0012 (plugin hosting)
- Wire: no protocol change, no format change.

## Context

`GetState` with a `save_dir` lets the host copy the files a plugin's state
refers to (sampler streams, convolution IRs) into the session. Four gaps
remain around `kStateCopyFreeMarginBytes` (512 MiB), `kStateReplyTimeoutMs`
(60 s) and the Plugin Files a session holds:

- Several hosts saving at once each see the same free space and each spend
  the margin, so together they can fill the disk below it.
- A Plugin File damaged on the stick is only found when a plugin loads it,
  mid-session.
- `copy_in` into a path that already holds a file could overwrite a good file
  or trust a bad one.
- A counted `Fault{Plugin}` is a number; the operator is not told.

ADR-0029 section 6 promises that a plugin never learns an absolute path.

## Decision

1. **One GetState with copying at a time.** The engine's plugin client
   serialises `GetState` requests that may copy files (`save_dir` set, no
   `kGetStateNoCopy`) across all hosts. The 512 MiB free-space margin is
   therefore checked against the space left by the previous save, and shared.
   `kGetStateNoCopy` requests are not serialised.
2. **Verify on open.** When a session is opened, every Plugin File is hashed
   off the audio thread and compared with its recorded hash. A mismatch or a
   missing file marks that instance's state as damaged and raises a held
   operator notice; the instance loads without it, never with the bad file.
3. **`copy_in` keeps a verifying existing file.** If the destination exists
   and hashes to the expected value, it is kept and nothing is written. If it
   exists and does not match, the copy fails with `StateStatus::Failed`; it is
   never overwritten.
4. **The State reply waits `kStateReplyTimeoutMs`** when
   `state_reply_timeout_ms` says so, and `kReplyTimeoutMs` otherwise. A
   timeout counts as a fault and kills the host as today.
5. **A counted `Fault{Plugin}` becomes a held operator notice** naming the
   plugin and the instance, not the 2.5 s status line.

### The ADR-0029 section 6 limit

The guarantee that no absolute path reaches the plugin holds only for paths
that pass through `absolute_path`. A plugin that builds a path from its own
configuration, an environment variable or a directory it scanned is outside
it. Section 6 is a mapping layer, not a sandbox; the sandbox is the process
boundary and the host's file permissions.

### Not decided here

- The "collect plugin files" action (gathering files a session references
  outside its folder).
- Reflink or clone copies in place of a byte copy.
- The preset store's `save_dir`.
- The probe's scratch folder.
- USB stick throughput against the 60 s budget, and FAT 0444 (read-only)
  permission behaviour: both need hardware.

## Rationale

A shared margin is only a margin if one writer spends it. Verifying at open
moves a failure from mid-take to before the take. Refusing to overwrite keeps
a possibly good file over a guess. A fault the operator cannot see is the
"limping silently" the host rules forbid.

## Consequences

- Saving a session with several sampler plugins is sequential; the worst case
  is the sum of the copies, each inside its own 60 s.
- Opening a session with many large Plugin Files takes the time to hash them.
- The plugin client gains a save queue and a notice call; no wire change.
- Tests for each point come with the implementation PR.

## Alternatives rejected

- **Parallel saves with a per-host share of the margin**: splits free space
  by guess and still races with other writers.
- **Verify on first load only**: finds damage mid-session.
- **Overwrite on `copy_in`**: replaces a verified file with an unverified one.
- **Raise the margin instead of serialising**: wastes space and still races.
- **Keep the fault on the status line**: 2.5 s is too short for a refusal.
