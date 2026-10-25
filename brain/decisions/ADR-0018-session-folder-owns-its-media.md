# ADR-0018 — The session folder owns its media: the engine records, flushes and reads in the open session's folder

- Status: **Accepted**, 2026-10-25, by the coordinator on the recommended
  defaults (owner's standing rule), including O1–O3 as recommended.
- Date: 2026-10-25
- Proposed by: architect, from the root cause the code review of PR #131
  found (B1, #issuecomment-6401920193): plugin states and takes are
  written to the engine's `--dir`, the session file to the folder the
  interface chose.
- Amends, if accepted: `session-format.md` ("What a session is on disk",
  "Saving", "Crash recovery", "Opening"; the file format itself does not
  change), the `ModelAction::Load` and `ModelAction::Save` contracts in
  `contracts/api` (one protocol bump), `engine/app`'s command line and the
  `sonora-session.service` unit, #131's state copy at save (it becomes one
  case of §5's gather) and #132's orphan scan (§6).
- Numbering: follows ADR-0017.

## Summary

*Navigation only; the sections below are the decision.*

- §1: the engine has one **active session folder**. Takes, wrap sidecars,
  plugin state flushes and source reads all resolve against it. It changes
  only on the control thread and only while no take is open or closing.
- §2: `Load` carries the session file's absolute path; the engine enters
  that folder (lock, recovery, medium check) before the document swaps.
  Loading while a take is open or closing is refused.
- §3: a new session gets its own visible folder at once,
  `<root>/Untitled <n>/`. The first Save renames that folder when the
  target is on the same filesystem; otherwise it is a Save As.
- §4: a standing `SessionFolder` event tells every client where the
  session lives.
- §5: Save As makes a self-contained folder: a hard link on the same
  filesystem, a durable copy across volumes, never a reference outside the
  folder. Refused while recording.
- §6: crash recovery and the orphan offer scan the active folder. #132's
  session-name filter goes, because one folder now holds one session.
- §7: existing sessions: an explicit "gather" offer pulls the files they
  name from the old shared folder. No session format bump.
- §8: the medium is checked when the folder is entered and before every
  take, so a full or read-only medium is refused at record start, not
  partway through a take. Preallocation waits for a measurement.
- §9: protocol bump (the next free number at landing); session format
  unchanged.

## Context

`session-format.md` already says the folder is the unit. Every path in
the file is relative to it, and "a session never points outside its own
folder, because the folder must survive being moved to another stick".
The code does not do this:

- `engine/app/src/main.cpp` fixes `SessionConfig::dir` at start
  (`--dir`, default `~/Sessions/Untitled`; the image's unit passes the
  same path). `Session::begin_take` opens every take in
  `<dir>/Audio Files`. `PlaybackController` resolves source paths against
  it. `PluginHosting` reads and, through `follow_flushes()`, writes
  `Plugin State/` there.
- `ModelAction::Load` carries bytes and no path ("so the engine needs no
  view of the interface's filesystem"). The engine never learns where the
  session it opened lives.
- `ModelAction::Save` (protocol 11) carries an absolute path in the
  engine's filesystem, and the saver writes the session file and `MIDI
  Sequences/` there (ADR-0015 update of 2026-10-25). The interface picks
  that path: `SessionModel::writeSession` uses
  `<sessions_dir_>/<Name>/<Name>.sonora`.

So the session file and `MIDI Sequences/` go to `~/Sessions/Mix/`, while
`Audio Files/` and `Plugin State/` go to `~/Sessions/Untitled/`. That
folder is shared by every session the engine ever opened. The failures:

1. Copy `~/Sessions/Mix` to another stick: every take is offline, and
   every plugin opens at its defaults.
2. Start the engine with `--dir ~/Sessions/Mix`, or open Mix from an
   engine whose `--dir` differs: the same thing.
3. Open Mix from the default engine: playback reads
   `~/Sessions/Untitled/Audio Files/Audio 1_01.wav`. That file is
   whichever session last recorded "take 1 on Audio 1" in the shared
   folder. The take numbers come from that folder (`next_free_take`), so
   collisions are rarer than they could be, but nothing ties a file to
   its session.
4. #132 had to filter orphans by the session name in each take's
   Description, because the shared folder holds the takes of every
   session. That filter breaks as soon as a session is renamed.

#131's fix copies the named state files into the session folder at Save.
That repairs plugin state for that PR. It cannot repair takes the same
way: copying every take at every first save doubles the disk writes on a
stick. A take recorded before the first save also exists only in the
shared folder, and it is the take a crash would lose.

Pro Tools' model, which we reproduce as workflow (ADR-0004): a session is
created in a folder that holds its `Audio Files/` and `Session File
Backups/`, recording goes there, and "Save Copy In" produces a
self-contained copy. Its "Save As" keeps referencing the original audio.
We do not copy that part (§5, alternative A2).

## Decision

### 1. One active session folder, changed only between takes

The engine holds one **active session folder**, an absolute path in its
own filesystem. It replaces `SessionConfig::dir` as the base of every
relative path the engine resolves or writes:

- takes and their `.wraps` sidecars: `<folder>/Audio Files/`;
- `next_free_take()`: scans that folder only;
- source reads (`PlaybackController::add_source`, `source_of`,
  `place_file`);
- plugin state reads at host spawn and flush writes (`follow_flushes()`:
  `job.file` becomes the active folder);
- the saver's session file, sequence files and backups (already per
  file).

**The invariant:** the active folder changes only on the control thread,
and only while no take is open and none is closing. A closing take means
an occupied writer slot that the disk thread has not yet released. The
audio thread never sees a path. The disk thread holds only file
descriptors and the paths stored in its writers and sidecars, so it reads
no folder at all (checked: `disk_thread.cpp` does not touch the config).
Because of the invariant, a take always closes into the folder it was
opened in, and `AddSource` + `RecordTake` always name a file in the
document's own folder.

`Session::enter_folder(path, why)` runs on the control thread and takes
these steps in order. The first failure refuses and leaves the old folder
active:

1. Refuse if a take is open or closing.
2. `FolderLock::acquire(path)`. Busy: refuse, "another engine has this
   session open". This is make-before-break: the old lock is held until
   step 6.
3. Create `Audio Files/` if missing, and sync the parent the first time.
4. `recover_unclosed_takes(path)`. This repairs the headers of unfinished
   takes before anything can record there (session-format.md, "Crash
   recovery", now per folder).
5. The medium check (§8).
6. Swap the folder, release the old lock, and `rmdir` the old folder's
   `Audio Files/` and then the folder itself if it is an untitled folder
   with nothing in it. `rmdir` fails on a non-empty directory, so this can
   never delete a file.

The folder string is handed to readers by value, or as a
`shared_ptr<const std::string>`, never as a mutable member that another
thread reads.

### 2. `Load` carries the path; no load during a take

`ModelAction::Load`, and the Load that follows `LoadPart`s, gains the
session file's absolute path in the engine's filesystem. This is the same
assumption `Save` already makes: the control socket is local, so the
engine and the interface see one filesystem. The engine:

- refuses the Load while a take is open or closing, with "stop recording
  before opening another session". Today a Load during a take is accepted
  and the take lands in the new document (#132 defers its scan for that).
  That puts one session's take into another session. It is gone;
- runs `enter_folder(session_folder_of(path))`, which handles a file
  opened from `Session File Backups/`, before the document swaps. A
  refusal keeps the old session, its document and its folder, as a
  refused Load does today;
- checks that `path` exists and is a regular file. It does not compare
  bytes, because the bytes may be a migrated autosave or a bundle.

An empty path means a **new session** (§3).

### 3. New sessions: a visible folder from the first moment; the first Save renames it

The engine gains `--sessions ROOT` (default `~/Sessions`, the ADR-0005
location on the image). At start-up, and at each Load with an empty
path, it creates `ROOT/Untitled/`, or the first free name of
`Untitled 2`, `Untitled 3` and so on (the interface's
`unused_session_name` rule), and enters it. Each new session gets its own
folder, so no two sessions ever share one.

`--dir PATH` stays and means "start in this folder". The headless
`--record` mode, the tests and `tools/*` keep working unchanged.
`sonora-session.service` switches to `--sessions /home/sonora/Sessions`.

**First Save.** The interface asks to save `ROOT/Mix/Mix.sonora` while
the active folder is an untitled folder that has never held a session
file. If the target folder does not exist and its parent is on the same
filesystem (`st_dev` equal), the saver `rename(2)`s the active folder to
`ROOT/Mix`, syncs `ROOT`, then writes the session file as for any Save.
The rename is atomic. The `flock` stays on the inode. Source paths are
relative, so the document does not change. Anywhere else (another
volume, an existing folder) the first Save is a Save As (§5).

A first Save that would rename the folder is **refused while a take is
open or closing**: "stop recording to name the session; the take is
being written to Untitled 2". Nothing is lost: the take is already in its
session's folder. A Save without a rename, into the active folder, works
while recording as today (paced behind a checkpoint).

**After a crash.** An `Untitled <n>` folder with takes and no session file
is an unsaved recording. The interface's Open browser lists such folders
under the sessions root as "Unsaved recordings", since it sees the same
filesystem. Opening one is a Load of an empty session with that folder's
path. Recovery runs on entry (§1 step 4) and the orphan offer (§6) places
the takes. The engine never reuses such a folder for a new session,
because its name is taken.

### 4. The engine says where the session is

A new standing event, `EventKind::SessionFolder`, carries the active
folder's absolute path and flags: `untitled`, `read_only`, and the
medium's filesystem kind and free bytes at entry. It is replayed to late
clients and replaced at every folder change, the same pattern as #132's
`RecoveryOffer`. The interface builds Save paths from it and stops
assuming its own `sessions_dir_`. `SONORA_SESSIONS_DIR` stays for
`FakeEngine` runs and tests.

### 5. Save As: self-contained, linked or copied, never referenced

Save As is a Save whose target folder is not the active folder (other
than §3's rename). The saver thread:

1. Refuses while a take is open or closing. Refuses a target folder that
   exists and is not empty, unless it is the active folder.
2. Lists every file the document names: every source in the pool, used
   or not (undo and comping can bring an unused one back; owner question
   O2), every `Plugin State/` file and every `MIDI Sequences/` file. Files
   of `MIDI Files/` that sources name are included. `Session File
   Backups/` is not copied; the new session starts its own history.
3. Same filesystem as the source folder: `link(2)` each file. Rule 1 of
   the format (a closed audio file is never modified) is what makes a
   shared inode safe. Deleting from one session only drops that
   session's link. Where `link` fails (`EPERM`, `EXDEV`, or a filesystem
   without hard links), the file is copied instead.
4. Otherwise, copy. First compare the total size with `statvfs` free
   space and refuse with both figures if it does not fit. Each file is
   written as `<name>.part`, `fdatasync`ed and renamed. Each directory is
   synced once at the end. A `.part` left by a crash is deleted when the
   saver next enters that folder, the same rule as `.msq.tmp`. Progress
   goes out as transient Notices, one per file.
5. Writes the sequence files and the session file into the target by the
   existing steps. The session file is written **last**, so a folder
   without one is an interrupted Save As. It is never mistaken for a
   session, and it is listed among the unsaved recordings.
6. Asks the control thread to `enter_folder(target)`. Takes recorded from
   then on go to the new folder.

The source folder is never modified or deleted. That includes an
untitled folder copied to another volume by its first Save: it stays
and is listed as an unsaved recording (owner question O1). #131's state
copy becomes step 3/4 for `Plugin State/`. Its tests carry over.

### 6. Crash recovery and the orphan offer follow the folder

- Header recovery runs at each folder entry (§1 step 4), not only at
  start-up over `--dir`. The FolderLock covers it, as before.
- `find_orphan_takes()` (#132) scans the active folder's `Audio Files/`.
  The **session-name filter is removed**. One folder holds one session,
  and a renamed session (§3) carries its old name in every take's
  Description, so the filter would hide that session's own orphans. The
  originator, rate and "holds audio" filters stay.
- With no Load during a take (§2), #132's deferred scan
  (`orphan_scan_pending_`) has nothing left to defer. It is removed.

### 7. Existing sessions: an explicit gather, no format bump

The format already says that media are in the folder, so no file changes
meaning and `sonora_session` does not move. What is wrong is where earlier
engines put the files. At open (session-format.md "Opening", after
step 4), the engine looks at each offline source and each missing state
file in the **legacy folder**: `--legacy-dir`, default
`~/Sessions/Untitled`, the old shared default. It accepts a file only if
that file is at the same relative path and matches by identity:

- sources: the UMID when both have one; otherwise channels, length,
  `TimeReference` and the Description's `take=` and `track=` all equal;
- states: the SHA-256 of the bytes equals the name.

Matches are offered once as a standing `GatherOffer`: "12 files of this
session are in the old shared folder (3.1 GB). Gather into the session?"
Accepting runs §5's link-or-copy for those files, and the sources come
online. Declining changes nothing, and the next open offers again. The
legacy folder is never modified, because other sessions' files are in it.
The product has not shipped, so this exists for the owner's development
sessions. It is also the path a session copied from an older stick
takes.

### 8. The medium, checked before recording starts

When the engine enters a folder (§1 step 5), and again at every take
start (control thread, before any file opens):

- **Read-only** (`statvfs` `ST_RDONLY`, or `access(W_OK)` fails): the
  folder is entered, playback works, the standing event says
  `read_only`, and arming to record is refused: "this session's medium
  is read-only; Save As to record". No probe file is written; the
  FolderLock already promises not to write.
- **Free space:** a take start is refused when the free bytes are below
  the armed channels × bytes per frame × rate × the reserve (placeholder
  **10 minutes**, owner question O3). The refusal gives both figures. The
  disk thread reads `statvfs` after each checkpoint (it is not the audio
  thread and may make syscalls) and posts a warning below 5 minutes. It
  never stops a take: a full medium already fails the take loudly at the
  short write.
- **Filesystem kind:** vfat is reported. A take on vfat stops at 4 GiB,
  about 8 hours per mono 24-bit track at 48 kHz. That limit sits below
  the RIFF limit the writer already has (`brain/research/bwf-interop.md`).
- **Preallocation: not now.** `fallocate(FALLOC_FL_KEEP_SIZE)` per take
  could reduce extent allocation and journal work during checkpoints. It
  also leaves `st_size` alone, so recovery by file size still holds.
  Whether it lowers `longest_checkpoint_ns` on a stick is Unverified 2.
  If it lands, it must use `fallocate(2)` and never `posix_fallocate`,
  whose glibc fallback writes zeros for the whole length on vfat. It must
  run on the control thread at open or on the disk thread after a
  checkpoint, never on the audio thread. A writer's `close()` and
  recovery must truncate back to the data, and recovery is the one write
  to a take that rule 1 allows.

### 9. Versions

- **Protocol:** one bump, the next free number at landing (12 is #132's).
  It covers `Load`'s path, the `SessionFolder` and `GatherOffer` events,
  the `session.gather_media` and `session.leave_gather` commands, Save's
  rename and Save As semantics, and Load refused during a take. Every pin
  moves together: `kProtocolVersion`, `test_wire`, and the `wire.cpp`
  event range.
- **Session format:** unchanged (§7).
- **`common/platform`** gains `hard_link`, `free_bytes`, `read_only`,
  `filesystem_kind` and `rename_dir_durably`. POSIX now. The Windows
  equivalents (`CreateHardLinkW`, `GetDiskFreeSpaceExW`, `MoveFileExW`)
  are noted for ADR-0008 and not built. The gather lives in
  `common/session_files`, so the engine and the interface's tests share
  it, as ADR-0007's update of 2026-10-25 set out.

## Rationale

- **It fixes the cause, not each symptom.** One rule, "every path is
  relative to the active folder, and the folder changes only between
  takes", covers takes, states, playback, orphans and the
  copy-to-another-stick case. #131's copy-at-save fixed one of those
  five.
- **No take ever exists only outside its session.** Takes are written in
  the session's folder from the first frame, including before the first
  save, so a crash leaves them where the session, or the unsaved
  recordings list, will find them.
- **Refusals happen at the edges, never mid-take.** A Load, a naming
  Save, a Save As, a busy lock, a read-only medium and a full medium are
  all refused before a take starts, or while none is running. Nothing in
  this design changes behaviour while a take is recording.
- **The real-time path is untouched.** The audio thread never had a
  path. The disk thread gains one `statvfs` per checkpoint. Everything
  else runs on the control or saver thread, which may block.
- **Cheap on a stick.** The first save is one `rename`. Save As on the
  same volume is links. Only a real cross-volume copy pays I/O, and it
  is refused up front when it would not fit.

## Consequences

- An operator cannot open another session, or name an untitled one,
  while recording. Pro Tools has the same restriction for Open. It
  costs one Stop.
- Untitled folders pile up in `~/Sessions` after crashes and declined
  saves. They are visible and listed, and empty ones are removed. Any
  other cleanup is an explicit operator action.
- Hard-linked sessions share inodes. A third-party tool that edits a WAV
  in place edits it in both sessions. Our own code never does that
  (rule 1).
- A Save As across volumes can take minutes on the saver thread. A
  normal Save queued behind it waits, and the progress Notices say why.
  Cancel is not in this ADR.
- #132 lands as it is. Step 3 below removes its filter and its deferral.
  #131's copy is reused, not thrown away.

## Unverified (for the researcher)

1. **`link(2)` on the persistence overlay.** Does a hard link in
   `/home/sonora/Sessions` (overlay upper on ext4, `home/rw`) succeed,
   give `st_nlink == 2`, and survive a reboot? The copy fallback makes a
   failure safe, but slow. Settles: whether same-volume Save As is
   instant on the image.
2. **Preallocation versus checkpoint latency.** On a physical stick, 18
   mono tracks, 10 minutes: `longest_checkpoint_ns` with and without
   `fallocate(KEEP_SIZE)` in 64 MiB steps. Settles §8's "not now". This
   moves to OS 1 with the other hardware measurements; a laptop proxy
   with a USB stick is acceptable before then.
3. **Copy throughput for Save As,** stick to stick and stick to laptop
   disk, with `fdatasync` per file. Settles whether Save As needs a
   cancel and a better progress display.
4. **`flock` and `rename` of a directory on exFAT and vfat sticks**
   mounted by udisks. Expected to work, since `flock` is VFS-level and
   local, but it is not tested.

## For the owner

Decided 2026-10-25 by the coordinator: O1, O2 and O3 as recommended.

- **O1.** After an untitled session's first Save copies it to another
  volume, is the untitled folder kept? *Recommended: keep it and list it
  as an unsaved recording.* Deleting audio is never a side effect.
- **O2.** Does Save As take unused sources? *Recommended: yes, the whole
  pool.* A "used only" option belongs to a later Save Copy In, as in Pro
  Tools.
- **O3.** The free-space reserve at take start. *Recommended: 10
  minutes for the armed channels,* revisited after Unverified 2 and 3.

## Implementation checklist (one PR each, in order)

1. **Engine: the active folder** (`engine/core`, `engine/control`,
   `common/platform`). `Session::enter_folder` with §1's steps and
   invariant. Playback, plugin hosting and flushes read the folder from
   the session. `free_bytes` and `read_only`. Tests, each failing first:
   refused while a take is open and while one is closing; a busy lock is
   refused and the old folder stays; recovery runs on entry; a take after
   a switch lands in the new folder; a flush writes `Plugin State/` into
   the active folder. No protocol change.
2. **Protocol: `Load` carries the path; `SessionFolder`** (`contracts/
   api`, `engine/control`, `ui/qt`, `FakeEngine`). The bump. The
   interface sends the opened file's path and builds Save paths from the
   event. Load during a take is refused. New conformance check, on both
   engines: "a take recorded after Load is in the loaded session's
   `Audio Files/`".
3. **Orphans per folder** (`engine/control`). Remove #132's name filter
   and its deferral. Test: a renamed session's orphan is offered.
4. **Untitled sessions** (`engine/app`, `engine/control`, `ui/qt`,
   `iso/` unit, `tools/`). `--sessions`; a fresh `Untitled <n>` at start
   and on a new session; empty folders removed; the first Save renames
   the folder, refused during a take; "Unsaved recordings" in Open.
5. **Save As** (`common/session_files`, `engine/control`, `ui/qt`,
   `keymaps/`). §5 gather: link or copy, `.part` cleanup, space check,
   progress, refused while recording. The Save As command in both key
   maps, per `brain/research/protools-logic-key-bindings.md`. #131's
   state copy folds in.
6. **Gather for existing sessions** (`engine/control`, `ui/qt`).
   `--legacy-dir`, identity matching, `GatherOffer`, and the two
   commands; reuses step 5.
7. **Medium checks at take start** (`engine/core`, `engine/control`).
   §8's read-only and free-space refusals and the low-space warning.
   Preallocation only after Unverified 2.

`session-format.md` is updated in the PR that changes each behaviour,
not ahead of it.

## Alternatives rejected

- **A1: keep the shared engine folder and copy at every save,** which
  generalises #131's fix to takes. Every take is written twice on a
  stick. A take recorded before the first save, the one a crash loses,
  lives only in the shared folder. Playback of an opened session still
  reads the shared folder, and #132's cross-session ambiguity stays.
  Rejected: it treats the symptom, and it costs I/O at the moment a
  save competes with the disk thread.
- **A2: let sources point outside the folder** (Pro Tools' Save As
  references the original audio). That is the exact failure this ADR
  exists for: the folder moves to another stick and the media stay
  behind. It also breaks the spec's one-folder invariant. Rejected.
- **A3: `Load` becomes a path, and the engine reads the session
  itself.** It is cleaner if the interface is ever remote. It would mean
  rewriting protocol 10's Load in parts (#120), which has just landed,
  and moving File > Open's errors, migration notes and offline lists
  into engine events. Rejected for now. Carrying the path (§2) keeps
  this option open.
- **A4: require a name and location before the first take** (Pro Tools'
  New Session dialog). It removes the rename and the untitled folders,
  but the appliance boots straight to recording and a dialog before the
  first take costs the operator. The rename is one atomic call.
  Rejected.
- **A5: hidden scratch folders for untitled sessions**
  (`~/Sessions/.untitled/<id>`). After a crash, the takes would sit
  where nobody looks. Rejected: visible folders appear in Open.
- **A6: Save As always copies, even on one volume.** It doubles disk use
  for no safety gain, because closed takes are immutable (rule 1) and an
  unfinished take cannot exist at Save As time (refused while recording,
  repaired on folder entry). Rejected. Copy stays the fallback when
  `link` fails.
- **A7: allow Load during a take and close the take into the old
  folder.** The take would end up in a session that is no longer open,
  with no document to place it in except through the orphan offer,
  which is a silent demotion of a take being recorded. Rejected: refusing
  costs one Stop.
