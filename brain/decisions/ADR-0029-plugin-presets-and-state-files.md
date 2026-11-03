# ADR-0029 — Plugin presets, and LV2 states that hold files

- Status: **Accepted**, 2026-11-02, by the coordinator on the recommended
  defaults below (no owner available; every open question carries the
  default taken).
- Date: 2026-11-02
- Proposed by: architect, for backlog item `plugins.host.state` (Track 4
  item 5: "LV2 states that hold files" and "Presets").
- Extends: ADR-0012 (§4 HostControl, §5.3 make-before-break, §6.1 state
  files, §8 control API), ADR-0018 (the session folder owns its media),
  ADR-0009 (undo; the owner's 2026-10-14 mix-and-plug-in undo history,
  BACKLOG Track 2 item 6), ADR-0004 (no proprietary formats).
- Version impact: host wire `kHostWireVersion` 3 → **4** (slice 1);
  session format +1 twice (slices 2 and 5); control protocol +1 twice
  (slices 2 and 5). Numbers are the next free ones at landing, one integer
  per change (ADR-0012 §6.2's rule): today they are wire 3, session 14,
  protocol 37.

## Summary

*Navigation only; the sections below are the decision. `grep -n '^#'` gives
each section's line.*

- §1: where a preset lives. User presets in `$XDG_DATA_HOME/sonora/presets/`
  (data, never the regenerable cache). Factory presets stay in the plugin's
  own bundle (LV2 `pset:Preset`, VST3 program lists and `.vstpreset`), read
  only. Curated content presets in `/usr/share/sonora/presets/`, read only.
  A preset a session uses is copied into the session folder.
- §2: the preset format. It is the plugin's own `StateBlob` (ADR-0012 §6.1,
  byte for byte the `Plugin State/` encoding) and a canonical-JSON sidecar
  holding the id, name, plugin identity, version, params and hashes. File
  names never carry the preset's name, so renames and plugin renames cost
  nothing.
- §3: how a load travels. A client sends `ModelAction::LoadPreset`. The
  engine resolves it, flushes the insert's current state, copies the preset
  into the session folder, and restarts the chain make-before-break with
  the preset as the instance's state. Only once the new host reports the
  state accepted does it apply one new operation, `LoadInsertPreset`. That
  operation carries the values before and after, so it is its own inverse
  and makes exactly one step in the mix-and-plug-in undo history. The audio
  thread sees only the plan swap it already sees for a chain edit.
- §4: compatibility. Identity (format + id) must match. Version is
  diagnostic. If the plugin refuses the state, the load is abandoned and
  the old host keeps playing. A missing plugin keeps its blob and shows
  "plugin missing". A damaged preset is listed as damaged and never loaded.
- §5: interchange. We read the published plugin-format preset formats (LV2
  `pset`, VST3 `.vstpreset` and program lists) and write only our own. No
  other DAW's preset format, no VST2 `.fxp`/`.fxb`.
- §6: LV2 `state:makePath`, `state:mapPath` and `state:freePath`. Scratch
  files live outside the session folder, per engine run and insert. On
  save, every file a state names is linked or copied into
  `<session>/Plugin Files/<sha256>/<basename>`, content-addressed and
  append-only, and the state names it relative to the session folder.
  `Load` carries the session folder and a scratch folder per instance, and
  `GetState` carries the folder to save into. All of it runs on the host's
  non-RT threads. A call from the processing thread is refused and counted.
- §7: version and protocol numbers. §8: the implementation slices, in
  order. Open questions with the defaults taken. Unverified items, handed
  to the researcher.

## Context

ADR-0012 gives a plugin's state a home in the session: `GetState` →
`StateBlob` → `Plugin State/<sha256>.state`, restored only in `Load`,
before the processing thread starts. It is silent on two things the Track 4
agent hit while implementing `plugins.host.state`:

1. **Presets.** Nothing says where a preset is stored, what it is, how a
   client asks for one, whether loading one is undoable, or what happens
   when the plugin changed version. The BACKLOG line ("user presets saved
   as StateBlobs beside the catalog cache; a load is a make-before-break
   restart") is a sketch. It also puts user data in `~/.cache/sonora/`,
   which ADR-0012 Q2 defines as regenerable and safe to delete.
2. **LV2 states that hold files.** `lv2_host.cpp` calls
   `lilv_state_new_from_instance` with every directory null and offers no
   `state:mapPath`/`makePath`/`freePath`. A sampler, convolver or looper
   that keeps a file therefore saves an absolute path into some folder the
   session does not own, or fails to save. That breaks ADR-0018 ("never
   references outside the folder"): the session plays on this machine and
   loses the sound on any other machine, or after the user tidies their
   home folder.

Constraints that settle most of the choices below:

- **The audio thread never sees any of this.** ADR-0012 §5.3 already has
  one path for replacing what a host runs: a new host, `Ready`, plan swap,
  old host retired. Presets must use that path, not open a second one.
- **Mix values are outside edit undo** (ADR-0009, ADR-0012 §6.1,
  `operations.h` "inserts"). The owner decided on 2026-10-14 that they get
  their own undo history (BACKLOG Track 2 item 6, not built yet). A preset
  load is a values change, so it belongs there, not in edit undo.
- **A take is never put at risk.** File copies big enough to compete with
  the disk thread do not run while a take is open.
- **Presets are user data.** Losing a user's presets is the plugin
  equivalent of losing a take. They do not go in a cache directory, and
  nothing deletes a preset whose plugin is missing.

## Decision

### §1 Where presets live

| Source | Location | Writable | Listed as |
|---|---|---|---|
| User | `$XDG_DATA_HOME/sonora/presets/` (default `~/.local/share/sonora/presets/`; on the appliance, the persistence partition's home, ADR-0005) | yes, by the engine only | "User" |
| Factory, LV2 | the plugin's bundle: `pset:Preset` resources lilv relates to the plugin (`lilv_plugin_get_related`) | no | "Factory" |
| Factory, VST3 | the plugin's program lists (`IUnitInfo`, the `kIsProgramChange` parameter); `.vstpreset` files in the VST3 preset folders the SDK documents for Linux (`~/.vst3/presets/<vendor>/<plugin>/`, `/usr/share/vst3/presets/…`, `/usr/local/share/vst3/presets/…`) | no | "Factory" |
| Content | `/usr/share/sonora/presets/` (plugins/content, curated; shipped in the image) | no | "Sonora" |
| Session copy | `<session>/Plugin State/` and `<session>/Plugin Files/` (ADR-0012 §6.1, §6 below) | by the engine | the insert's current preset name |

- **Our native plugins** (ADR-0011) are standard LV2/VST3 plugins, so their
  factory presets are DPF programs exported as LV2 `pset:Preset` in their
  own bundle, read by the same path as anyone else's. No third mechanism.
- **The engine owns the preset store.** It is the only process that can
  take a plugin's state (`GetState`), and it already owns the catalog and
  its cache. The UI never reads or writes preset files. On Windows (ADR-0008,
  later) the user store is `%APPDATA%\Sonora\presets\`, through
  `common/platform`.
- **A session never depends on the store.** Loading a preset copies its
  blob and files into the session folder (§3 step 3). Deleting or editing
  the user preset afterwards changes nothing in a saved session, and a
  session carried to another machine plays as saved.

### §2 The preset format

One directory per plugin, keyed so the directory name survives any rename:

```
$XDG_DATA_HOME/sonora/presets/
  <plugin-key>/                      32 lowercase hex: sha256("<format>:<id>"), first 128 bits
    plugin.json                      { "format", "id", "name", "vendor" } as last seen; for listing it when missing
    <preset-id>.preset.json          the sidecar: one per preset
    Plugin State/<sha256>.state      the StateBlob, exactly as in a session folder
    Plugin Files/<sha256>/<basename> files the state names (§6)
```

The sidecar, canonical strict JSON through `common/json` (ADR-0009):

```json
{
  "sonora_preset": 1,
  "id": "u-7f3a2c9e4b1d4e0f8a6b5c4d3e2f1a0b",
  "name": "Lead Vocal Bright",
  "plugin": { "format": "lv2", "id": "http://gareus.org/oss/lv2/dpl#mono",
              "name": "x42-dpl Digital Peak Limiter Mono", "version": "0.9.2" },
  "params": [ { "key": "threshold", "value": -1.0 } ],
  "state": { "file": "Plugin State/3f1c…e9.state", "sha256": "3f1c…e9", "bytes": 812,
             "files": [ { "path": "Plugin Files/a0b1…77/kick.wav", "sha256": "a0b1…77", "bytes": 482144 } ] },
  "category": ""
}
```

- **The blob is the plugin's own state, unparsed.** The bytes are the same
  `StateBlob` (`SNST`, version 1) that `GetState` returns and `Load` takes,
  so a preset becomes a session state by a file copy, with no conversion.
  For LV2 the blob leaves out port values (`lv2_host.cpp`: they are the
  table's), so **`params` is part of every preset**. A preset without
  `params` would restore a plugin's internal state with the knobs at their
  defaults.
- **Names live in the sidecar only.** Renaming a preset rewrites its
  sidecar (tmp, `fdatasync`, rename). Renaming a plugin's display name
  changes nothing on disk. The plugin key is derived from format and id,
  the identity ADR-0012 §6.1 already treats as stable.
- **Ids.** A user preset is `u-` followed by 128 random bits in hex. A
  factory or content preset is identified by where it came from:
  `lv2:<preset URI>`, `vst3-program:<unit id>/<program index>`,
  `vst3-file:<path relative to its VST3 preset root>`,
  `content:<id in its sidecar>`. Clients treat an id as opaque.
- **Names are unique per plugin within the user store**, compared
  case-sensitively after NFC normalisation. Save under a name that exists
  is refused unless the request names that preset's id (overwrite).
- **Hashes.** The sidecar's `sha256` of the blob and of each file is
  checked when a preset is loaded, not when it is listed. Listing reads
  sidecars only, so a plugin with 2,000 presets lists without reading 2,000
  blobs. A preset whose blob or file fails its hash is refused at load with
  "preset damaged", and is marked damaged in later lists.
- **Blobs are shared and append-only**, as in a session. Two presets with
  the same state share one `.state` file. Delete removes the sidecar, and
  the engine's store cleanup removes blobs and files no sidecar names.
  Cleanup runs when the store is first opened per engine run, never while a
  take is open.
- **The format is versioned** by `sonora_preset`. A reader refuses a
  version it does not know, naming it, and lists that preset as "made by a
  newer Sonora".

### §3 How a preset load travels

The client never sends an operation for this. It sends a request, and the
engine resolves it. Resolving needs the running plugin's state and files,
which only the engine can reach (ADR-0007 M7 item 6: resolution moves into
the engine):

```
UI ──ModelRequest{LoadPreset, op = encode_load_preset{insert, preset id}}──▶ engine control thread
```

On the control thread of `engine_control_server.cpp`, never on the audio
thread:

1. **Check.** The insert exists and is not missing. The preset's plugin
   format and id equal the insert's. No load for this insert is already
   pending; if one is, the newer request **supersedes** it, and the older
   one is answered Failed, "superseded". Stepping through presets with the
   arrow keys therefore costs at most one pending restart per insert.
2. **Flush the insert** (the `FlushPluginState` path for one insert), so
   the values the insert holds now are in the document as a `StateRef` and
   params. These become the operation's `prior`. If the host is down, the
   last state the document holds is the prior.
3. **Copy into the session.** The preset's `.state` goes to
   `<session>/Plugin State/<sha256>.state`, and each of its files to
   `<session>/Plugin Files/<sha256>/<basename>`. It is the same content
   address, so a link or copy is skipped when the target exists. The saver
   thread does it: `link(2)` on one filesystem, otherwise `.part`,
   `fdatasync` and rename (ADR-0018 §5). For a factory preset, the bytes
   come first from the one-shot reader (§5) and go through the saver the
   same way.
4. **Make before break.** A new host starts with the chain as it is, except
   that this instance's `LoadInstance::state` is the preset's blob and its
   initial parameter values are the preset's `params`. The other instances
   carry their state over through `GetState` (ADR-0012 §5.3). The old host
   keeps playing throughout.
5. **Commit or abandon.** When the new host's `Loaded` says
   `StateResult::Ok` for this instance, the engine checks that the insert
   still exists, still holds the same plugin, and has no values change
   since step 2. A change there abandons the load with "the insert changed
   while the preset loaded". If all holds, it applies **one operation**:

   ```cpp
   // Not edit-undoable: plug-in values (operations.h). One step in the
   // mix-and-plug-in undo history (BACKLOG Track 2 item 6). Its own inverse:
   // swapping prior and next undoes it, so the history stores the op alone.
   struct LoadInsertPreset {
       Id insert = 0;
       PresetRef preset;                    // {source, id, name}: display, and "modified"
       std::vector<ParamValue> params;      // next
       std::optional<StateRef> state;       // next
       PresetRef prior_preset;
       std::vector<ParamValue> prior_params;
       std::optional<StateRef> prior_state;
   };
   ```

   Every replica gets it as a `ModelUpdate`. The plan swap to the new host
   follows the normal `Ready` path. The reply is Ok after the update.
   - `Refused` for the state, a load timeout or a crash of the new host
     abandons the load: the new host is shut down and the old one was never
     stopped, so the audio and the document are as before. The reply is
     Failed, naming plugin, preset, the preset's recorded version and the
     running one. No standing fault is raised (unlike ADR-0012 Q4), because
     nothing is left running in a degraded state.
6. **What the audio thread sees:** one plan swap at a block edge, the swap
   it already performs for any chain edit. Nothing new allocates, locks or
   waits on the audio path. A preset load is **not sample-accurate** and is
   not automatable. It is audible from the first block after the swap, like
   an insert edit. Recording continues throughout (capture is pre-insert,
   ADR-0012 §5.3).

**Undo.** `LoadInsertPreset` is a values change, so it makes no edit-undo
step (`is_undoable` false, as for `SetInsertParams`). It makes exactly one
step in the mix-and-plug-in history once that history exists. Because the
operation carries `prior_*`, undo applies it with prior and next swapped:
the same operation, going through the same steps 3–5, with the prior state
already in the session folder. Undo can therefore never need a state the
session has lost. Until Track 2 item 6 lands, the plug-in window's preset
menu offers **Undo Load Preset**, which sends the inverse as a
`LoadPreset` of the prior values (Open question Q1). A `SetInsertParams`
or `SetInsertState` applied after a load sets `InsertMix::preset.modified`
(the "*" Pro Tools and Logic show beside a changed preset's name). A later
`LoadInsertPreset` clears it.

**Saving a user preset:**
`ModelRequest{SavePreset, op = encode_save_preset{insert, name, overwrite id or empty, category}}`.
The engine sends `GetState` with `save_dir` = the plugin's store directory,
so files go into the store's own `Plugin Files/` (§6). It takes the
insert's params from the document after the flush. The saver thread writes
the blob, then the sidecar (tmp, `fdatasync`, rename), then syncs the
directory. The reply is Ok with the new id, or Failed naming the cause. A
save is allowed during a take only when no file copy is needed (§6
`no_copy`). **Rename and delete:**
`ModelRequest{EditPreset, op = {id, rename <name> | delete}}`, for user
presets only; factory and content presets are read only. **Listing:**
`ModelRequest{ListPresets, op = "<format>:<id>"}`, answered by a new
engine → client message, `PluginPresetList{format, id, entries[]}`. Each
entry carries id, name, source, category, damaged and the version it was
made with. The same message goes to every client after any SavePreset or
EditPreset for that plugin, and after a rescan.

### §4 Compatibility

- **Identity must match.** The preset's format and id must equal the
  insert's. The host checks the StateBlob's identity too
  (`restore_state`: "the state is another plugin's"). There is no
  cross-plugin loading, not even between a plugin's LV2 and VST3 builds:
  their states are not one format.
- **Version is diagnostic** (as `PluginRef::version`). A preset made with
  another version is loaded, and the plugin decides. If it refuses, the
  load is abandoned (§3 step 5) and the reply shows both versions.
- **Unknown parameter keys** in a preset (a parameter the plugin no longer
  has) are kept in the document, as a session's are. The running instance
  ignores them, as the plan builder ignores a lane whose key does not
  resolve (ADR-0027 §5). The reply carries a count ("2 parameters this
  version does not have").
- **Missing plugin.** An insert whose plugin is missing keeps its blob,
  params and `preset` (ADR-0012 §6.1). Its preset menu shows the preset
  name and "plugin missing", and loads nothing. User presets of a plugin
  that is no longer installed stay in the store, listed under "Plugins not
  installed" (from `plugin.json`). Nothing deletes them automatically.
- **Damaged.** A blob or file whose hash does not match its sidecar is
  never loaded (§2).
- **Session-embedded copy wins.** A session opened on a machine without the
  user preset plays from its own `Plugin State/`. The `preset` reference is
  only a name.

### §5 Interchange limits

- **Read:** LV2 `pset:Preset` through lilv. A preset's port values become
  `params`, and the rest becomes the StateBlob's one section. VST3
  `.vstpreset` uses the SDK's published `PresetFile` layout (MIT since 3.8,
  `brain/research/plugin-host-vst3.md`): the `Comp` and `Cont` chunks become
  the StateBlob's two sections. The file's class id must match the
  plugin's. VST3 program lists: the program is selected in a one-shot host,
  then `getState` (Unverified 3). All of these are formats the plugin
  standards publish. None is a DAW's.
- **The reader runs in a one-shot process,** `sonora-plugin-host
  --read-preset <bundle> <preset id>`, spawned by the scan worker (ADR-0012
  amendment 2026-10-19), with the probe's timeout and crash rule. A preset
  file is untrusted input, and reading a VST3 program needs the plugin
  loaded. Neither belongs in the engine. It writes one StateBlob plus params
  to stdout, under `kMaxStateBytes`. **Listing** (LV2 preset URIs and
  labels, VST3 program names, `.vstpreset` paths) is part of the
  `--probe` record and the catalog cache. Content is read on demand.
- **Write:** only our own format (§2). We do not write `pset` bundles into
  `~/.lv2` or `.vstpreset` files into `~/.vst3/presets`. Other hosts would
  pick them up, and a file another program reads is a compatibility promise
  we have not decided to make (Q3).
- **Never:** Pro Tools `.tfx`, Logic `.pst`/`.cst`, `.aupreset`, VST2
  `.fxp`/`.fxb` (the VST2 SDK is withdrawn and its licence unverified), or
  any other DAW's preset or channel-strip format. Per CLAUDE.md and
  ADR-0004, interchange beyond the plugin standards is BWF, AAF and MIDI
  only. A request to import one is out of scope and is refused as such.

### §6 LV2 states that hold files: `makePath`, `mapPath`, `freePath`

**Where files live.**

| Kind | Folder | Lifetime |
|---|---|---|
| Scratch (`makePath`) | `$XDG_STATE_HOME/sonora/plugin-scratch/<engine run id>/<insert id>/` (default `~/.local/state/…`) | the engine run; swept at the next start |
| Saved (a state names it) | `<session>/Plugin Files/<sha256>/<basename>` | append-only, like sources; removed only by explicit cleanup |
| Preset | `<store>/<plugin-key>/Plugin Files/<sha256>/<basename>` | until no sidecar names it |

- **Scratch lives outside the session folder** because ADR-0018 renames an
  untitled folder at its first Save and moves the session at Save As. A
  plugin holding an absolute scratch path would lose it at either. The
  engine's own run directory never moves. It sits in the user's home, on
  the same filesystem as `~/Sessions` in the default layout, so saving is
  usually a `link(2)`.
- **Saved files are content-addressed** and never rewritten. A later state
  that changes a file gets a new hash and a new directory, so every state
  in the undo history and every older `Plugin State/` file still names the
  bytes it was saved with. The basename is kept because some plugins show
  it to the user.
- **The session names its files.** `StateRef` gains
  `files: [{path, sha256, bytes}]`, filled from the host's `State` reply.
  Save As (ADR-0018 §5 step 2) lists them beside the `.state` files, and
  link-or-copy covers them. The folder sync after a save (BACKLOG #131 N1)
  covers `Plugin Files/` too. Cleanup removes a `Plugin Files/<sha256>/`
  that no kept session file names, as for `Plugin State/`.

**The features the host offers** (LV2 State extension;
`plugins/host/src/lv2_host.cpp`):

- `state:makePath`, at instantiation. A relative path resolves under the
  instance's scratch folder, with parents created. An absolute path, a `..`
  component or a symlink out of the folder is refused: NULL is returned and
  counted as `Fault{Plugin}` text "makePath refused <path>" (Unverified 4).
- `state:mapPath`, in `save()` and `restore()`:
  - `abstract_path(abs)`. A file already under the session folder's
    `Plugin Files/` gives its path relative to the session folder. Any other
    file (scratch, or something the user loaded from `~/Samples`) is hashed
    and linked or copied into `<save_dir>/Plugin Files/<sha256>/<basename>`
    (tmp, `fdatasync`, rename; the directory is synced by the engine). Its
    relative path is returned, and it is recorded for the `State` reply. A
    path under a **previous** session folder this host was given (the
    untitled folder before its rename, the folder before Save As) is
    rewritten to the same relative path, so a plugin restored from the old
    folder saves cleanly into the new one.
  - `absolute_path(rel)` gives `<session_dir>/<rel>`. A path that escapes
    the folder gives NULL, counted as a fault. **Amended (#276 review N5):**
    an *absolute* path is returned unchanged. States saved before wire 4
    (lilv's own mapPath, no folders) hold absolute paths, and refusing them
    would break every such state. Reading through them was already possible
    in wire 3. The copy on save is new, so `abstract_path` names such a path
    exactly as it was given and **never copies it in**. Otherwise a received
    session that names `~/.ssh/id_rsa` would carry that file away in its own
    `Plugin Files/` at the next save. The host remembers each absolute path
    it passed through, as given and canonical, for the instance's life.
    Gathering an old session's outside files into the folder is an explicit
    operator action ("Collect plugin files"), not a side effect of saving.
    That action is not scheduled yet.
  - **Copy safety (#276 review N2–N4, #283 review).** `Plugin Files/` and
    `<sha256>/` are opened as real directories (`mkdirat`, then
    `openat(O_NOFOLLOW | O_DIRECTORY)`) before anything is made in them.
    After that the tmp file, the rename and the free-space query all go
    through those descriptors. A symlink at either one fails that save, and
    nothing is written through it. The tmp is `.incoming-<host pid>-<n>`,
    created `0444`. A saved file is read-only from the start and the rename
    keeps it so. The copy syncs every 64 MiB, and it stops with a Failed
    text once the save has spent `kStateCopyBudgetMs` (the State timeout
    less 15 s). A file in `Plugin Files/` is named by its directory's sha
    without being read only while its size, mtime and inode match what the
    host last saw: at `absolute_path`, or when it saved or hashed the file.
    Otherwise it is hashed again, and a file that no longer matches is
    copied as new bytes. During a take (`no_copy`) that check counts as a
    read, so the save fails "after the take". A file never seen by this
    host, for example in a received folder, is trusted at restore as
    before. Verifying a whole folder on open is the engine's job in slice 2.
  - **Stale tmp files.** At the host's Load (`session_dir`) and at the first
    save into a folder, the host removes `Plugin Files/.incoming-<pid>-<n>`
    whose pid no longer runs (`kill(pid, 0)` gives `ESRCH`), with `unlinkat`
    on regular files only. A live pid's file belongs to another host's copy
    in progress and stays. A recycled pid only delays the sweep. Any other
    name is left alone.
  - **One deadline at a time per host.** The host answers a chain's
    GetStates in order, so the engine starts a GetState's deadline only
    when the one before it on the same host has been answered. Each save
    has the whole State timeout to itself. A chain of N instances can take
    N times the timeout, and every one of them is bounded.
- `state:freePath` frees what the two above return (the host allocated
  it). Offered always. A plugin that frees with `free()` instead works
  anyway, because the host allocates with `malloc`.
- **Threads.** All three allocate and touch the filesystem. They run on the
  host's main or worker thread, the threads where LV2 calls `save()`,
  `restore()` and instantiation, never on the host's `SCHED_FIFO` processing
  thread. The host records that thread's id at start. A call from it is
  refused in constant time (an id compare and an atomic increment; nothing
  else), returns NULL, and is reported later as a counted
  `Fault{Plugin}`. A plugin that does file work in `run()` is broken, and
  we say so rather than stall the chain.
- **Durability and the take.** `GetState` gains `flags` with `no_copy`, set
  by the engine while a take is open or closing. With it, `abstract_path`
  copies nothing. A file not already in `Plugin Files/` makes that
  instance's save fail ("plugin files are saved after the take"). The flush
  then reports the insert Failed, the last saved state stands, and the next
  flush after the take copies. Hashing a scratch file that is already a
  link candidate is still a read. With `no_copy`, hashing is skipped too.
  No bulk I/O competes with the disk thread during a take.
- **Time.** A copy is bounded by `kStateReplyTimeoutMs`, a new constant
  for `GetState` replies (60 s, replacing `kReplyTimeoutMs`'s 5 s for this
  message only). The host's processing thread keeps bumping the heartbeat,
  so a long save is not mistaken for a hang. Whether 60 s covers a large
  sample set on a USB stick is Unverified 2.
- **Lilv.** The host passes its own `mapPath`/`makePath`/`freePath` to
  `save()` and `restore()`, and does not use lilv's scratch, copy, link and
  save directories, whose naming is not content-addressed. That lilv lets a
  host do this, and what `lilv_state_to_string` writes for an `atom:Path`
  property, is Unverified 1. It is the first thing slice 1 settles. If lilv
  forces its own file handling, slice 1 calls the plugin's
  `LV2_State_Interface` directly, with our own store and retrieve callbacks.
  That is more host code, and it is still plugin state the engine never
  parses.

**Protocol fields** (host wire 4, `contracts/plugin/host_control.h`):

- `Load` gains `session_dir` (absolute; may be empty for a session never
  saved and with no folder, when nothing is restored from files), and each
  `LoadInstance` gains `scratch_dir` (absolute, created by the engine).
  This is how a session load hands the folder to the host: the engine
  learns the folder from `ModelAction::Load`'s path (protocol 16, ADR-0018
  §2), so the client protocol needs nothing new for it.
- `GetState` gains `save_dir` (absolute: the active session folder, or a
  preset store directory) and `flags` (`kGetStateNoCopy`). Each one also
  updates the host's current session folder and adds the old one to the
  list `abstract_path` rewrites.
- `State` gains `files: [{path, sha256, bytes}]`, each path relative to
  `save_dir`, at most `kMaxStateFiles` (1,024) and every path under
  `Plugin Files/`. The engine checks each one exists and has that size
  before it names it in `StateRef`.
- VST3 is unaffected. Its state is two streams with no file feature. A
  VST3 plugin that keeps files names them inside its own stream, and we
  cannot see them (Consequences).

### §7 Versions

| Slice | Host wire | Session format | Protocol | Window format |
|---|---|---|---|---|
| 1 files in the host | 3 → 4 | — | — | — |
| 2 files in the session | — | +1 (`StateRef.files`; earlier versions migrate with none) | +1 (`StateRef.files` in ops and Snapshot) | unchanged (inserts in the window carry no `StateRef`; check at landing) |
| 5 preset ops | — | +1 (`InsertMix.preset {source, id, name, modified}`, optional) | +1 (`LoadPreset`, `SavePreset`, `EditPreset`, `ListPresets` = ModelAction 9–12; `PluginPresetList`; op `LoadInsertPreset`) | unchanged |

Preset sidecar `sonora_preset` 1 is new (slice 4) and versioned apart from
the session. The protocol comment block in `messages.h` gets one entry per
bump, in the existing style.

## Rationale

- **Make-before-break, not an in-place restore.** The old host plays until
  the new one is ready, so a preset load is gapless, and a plugin that
  hangs or crashes in `restore()` costs nothing audible. It is ADR-0012's
  one code path for crash, edit and load, so it adds no new state to the
  audio side and no new host message. What it costs later: a load takes as
  long as the slowest plugin of the chain takes to instantiate, and memory
  doubles for that chain briefly (ADR-0012 Unverified 5). A heavy sampler
  beside a light EQ makes the EQ's preset step slow. Supersession bounds
  the arrow-key case. If the measurement (Unverified 5) shows it is too
  slow, an in-place `RestoreState` over Reconfigure's pause is the
  amendment. It is rejected as the default below.
- **Commit after `Loaded`, not before.** ADR-0012 §8 replies once the
  document changes and lets audibility follow. For a preset, success means
  the plugin accepted the state. If we committed first, a refused preset
  would leave a document naming a preset the plugin is not running, and a
  later save would record a lie. Holding the commit costs one check (the
  insert unchanged since the flush) and a slower reply.
- **Engine-resolved request, client never sends the op.** Only the engine
  can flush the prior state and copy files into the folder it owns. A
  client-built `LoadInsertPreset` would carry a `prior` that may already be
  stale.
- **The plugin's own blob, byte for byte.** One encoding for session state
  and preset state means no converter and no second parser of plugin data,
  and a preset load is a file copy. The sidecar holds everything the user
  sees, so renames cost one small write.
- **Content-addressed files, scratch outside the folder.** Undo, Save As,
  the rename of an untitled folder and cleanup all work by the rules that
  already govern sources and `Plugin State/`. Nothing in the session folder
  is ever rewritten in place, which is what makes ADR-0018's hard links
  safe.

## Consequences

- `plugins/host` gains the three LV2 features, path checks and a hashing
  copier. It writes into the session folder for the first time. The engine
  still syncs the directory, and the files are content-addressed, so a race
  with the saver writes identical bytes at worst.
- A `GetState` can now take seconds. Its timeout splits from
  `kReplyTimeoutMs`. The flush reply waits for the slowest copy, so File >
  Save is slower on sessions with big plugin files the first time. Later
  saves link or skip.
- During a take, a plugin whose state gained a new file is not saved until
  the take ends. Autosave during a take can therefore hold an older state
  for that one insert, and its standing notice says so.
- VST3 plugins that store files by absolute path inside their own state
  stay non-portable. The VST3 format gives us no hook. This is documented
  in the plugin manager, not worked around.
- Two session format bumps and two protocol bumps. FakeEngine and the
  conformance suite change in slices 2 and 5.
- Track 2 item 6 (mix undo) gets a ready model of a plug-in value step: an
  operation that carries its own inverse.

## Alternatives rejected

- **In-place `RestoreState` on the running host** (pause the processing
  thread as Reconfigure does, `restore()` on the main thread, resume).
  Faster for heavy chains and no fork. Rejected as the default because it
  makes an audible gap (excused misses) for as long as `restore()` takes,
  and a plugin that hangs or crashes in `restore()` silences the chain
  instead of leaving the old one playing. It is also a second path beside
  make-before-break. Kept as the amendment if Unverified 5 says loads are
  too slow.
- **Preset load as an edit-undoable operation** (or as a `ReplaceInsert`
  of the same plugin, whose undo already restores values). Rejected
  because the owner put plug-in values in their own undo history
  (2026-10-14), and Ctrl+Z on an edit would then also flip a preset. The
  `ReplaceInsert` trick changes the insert's id on every load, which drops
  its automation lanes (ADR-0027 §5) and its standing faults.
- **Preset as a single file** (blob and metadata in one container).
  Renames rewrite megabytes, and the blob is no longer the byte-identical
  session state. Rejected.
- **User presets in `~/.cache/sonora/` beside the catalog cache** (the
  BACKLOG sketch). That directory is regenerable by ADR-0012 Q2, and users
  and tools delete caches. Rejected: presets are data.
- **Writing LV2 `pset` bundles or `.vstpreset` files** for other hosts.
  Rejected for now (Q3): it is a promise about files other programs read,
  and nothing in the product needs it.
- **lilv's own scratch, copy, link and save directories.** Their names are
  not content-addressed, and a copied file can be overwritten by a later
  save. Rejected unless Unverified 1 forces it.
- **Scratch inside the session folder.** It breaks at the first Save's
  rename and at Save As (ADR-0018 §3, §5). Rejected.
- **The engine copies external files after `State`.** `abstract_path` must
  return the final path during `save()`, and the blob is hashed with it.
  Deferring the copy would mean rewriting paths inside the plugin's state,
  and the engine does not parse plugin state. Rejected.

## Open questions, with the defaults taken

- **Q1. Ctrl+Z for a preset load before the mix undo history exists.**
  Default: no Ctrl+Z. The plug-in window's preset menu has **Undo Load
  Preset**, which sends the inverse. Once Track 2 item 6 lands,
  `LoadInsertPreset` is one step there and the menu item goes. Not chosen:
  holding presets back until item 6, which blocks Track 4 on Track 2.
- **Q2. Native plugins' factory presets: LV2 `pset` in the bundle, or our
  format in `/usr/share/sonora/presets/`?** Default: `pset` in the bundle
  (other hosts see them too, and one read path). Our format is for content
  presets only.
- **Q3. Export to `pset` or `.vstpreset`?** Default: no, until a product
  need appears. It would get its own ADR.
- **Q4. Preset load or save during a take.** Default: allowed when no file
  copy is needed. A preset that brings files the session lacks is refused
  during a take ("load it after the take"). The chain restart is safe
  while recording (ADR-0012 §5.3).
- **Q5. Pro Tools' "Set as User Default"** (the preset a new insert of
  that plugin starts with). Default: a `default` flag in the user store's
  `plugin.json` naming one preset id, applied by `AddInsert`'s resolver.
  This lands with slice 6, with no format change beyond `plugin.json`.
- **Q6. Name uniqueness and case.** Default: unique per plugin,
  case-sensitive after NFC normalisation, so "Bright" and "bright" may both
  exist. The UI warns before such a save.

## Unverified (for the researcher)

1. **Lilv and host-provided path features.** Does lilv (trixie's package)
   pass a host's own `state:mapPath`/`makePath`/`freePath` through
   `lilv_state_new_from_instance` and `lilv_state_restore` when every
   directory argument is null, and how does `lilv_state_to_string`
   serialise an `atom:Path` property (relative to the base URI, or
   absolute)? Settle it with a test plugin that saves one file, built in
   the container. This decides lilv versus direct `LV2_State_Interface` in
   slice 1.
2. **Copy throughput against the 60 s budget.** Measure sha256 plus link
   or copy of 100 MB, 1 GB and 4 GB of plugin files from home onto the
   persistence partition of a USB 3 stick, and onto an internal SSD. Done
   in the container for the code path, on hardware in OS 1.
3. **VST3 program lists through a one-shot host.** Does selecting a program
   through the `kIsProgramChange` parameter, then running one block and
   calling `getState`, give that program's state for real plugins (Surge
   XT, the LSP set, one plugin bridged by yabridge)?
4. **Plugins' behaviour when a path feature returns NULL**, and whether
   common file-bearing LV2 plugins (sfizz, the x42 convolver, ninjas2, the
   LSP samplers) modify files given to them at restore. They must not:
   saved files are content-addressed.
5. **Make-before-break time for a preset step** (ADR-0012 Unverified 5): a
   chain of one heavy sampler and one EQ, time from `LoadPreset` to the
   swap. If p95 is over about 1 s, revisit the in-place restore.

## Implementation slices, in order

Each slice is one PR, with the regression test written first and failing.
"Parallel" means relative to Track 1's automation work in
`engine/control/src/engine_control_server.cpp`, `plan_builder.cpp` and
`automation_writer.cpp` (ADR-0027 §8).

1. **LV2 path features in the host.** Files: `contracts/plugin`
   (`host_control.h/.cpp`: `Load.session_dir`, `LoadInstance.scratch_dir`,
   `GetState.save_dir/flags`, `State.files`, `kStateReplyTimeoutMs`,
   `kMaxStateFiles`), `plugins/host/src/lv2_host.cpp`, a new
   `plugins/host/src/state_paths.{h,cpp}`, the test plugin
   `urn:sonora:test#files`. Version: host wire 3 → 4. Test first:
   `plugins_host_state_files`. The test plugin makes a scratch file, saves,
   is restored in a new host from another folder, and reads the same bytes.
   Plus a `..` path refused, a call from the processing thread refused and
   counted, and `no_copy` failing that instance. Settles Unverified 1
   first. Review: Opus (contract). **Parallel: yes** (no engine_control
   files; the engine side sends empty folders until slice 2).
2. **Plugin files in the session.** Files: `common/model` (`StateRef.files`,
   session reader and writer, op codec), `brain/specs/session-format.md`,
   `engine/control/src/plugin_hosting.cpp` (scratch dirs, `save_dir`,
   `no_copy` during a take), `session_saver.cpp` and `active_folder.cpp`
   (Save As lists files, folder sync, cleanup), `contracts/api/messages.h`
   comment, FakeEngine, conformance. Version: session format +1, protocol
   +1. Test first: `session_folder_plugin_files`. Save, Save As across a
   simulated volume and the untitled rename each keep the file readable by
   a restored host, and a frozen version-14 fixture still loads. Review:
   Opus (storage, recovery). **Parallel: partly.** It touches
   `plugin_hosting.cpp`, which ADR-0027's lane resolution also touches, so
   land it between Track 1 PRs, not beside one.
3. **Factory preset discovery and the one-shot reader.** Files:
   `plugins/host/src/main.cpp` (`--read-preset`), `lv2_host.cpp` (`pset`
   listing), `vst3_host.cpp` (program lists, `.vstpreset` through
   `PresetFile`), `probe_record.h` (preset entries),
   `engine/control/src/plugin_scanner.cpp` and
   `plugin_catalog_cache.cpp`. Version: probe record and cache format +1;
   no client protocol. Test first: `plugins_host_read_preset` (eg-amp or
   LSP `pset`; a crafted `.vstpreset` with a wrong class id refused;
   truncated chunks refused) and a fuzz case for the `.vstpreset` reader.
   Settles Unverified 3. Review: Opus (untrusted parser in a contract
   record). **Parallel: yes.**
4. **The user preset store.** Files: a new
   `engine/control/src/preset_store.{h,cpp}` (layout, sidecar codec on
   `common/json`, hashes, tmp+rename, cleanup, damaged marking), tests.
   Version: `sonora_preset` 1. Test first: `preset_store`. Covers
   save/list/rename/delete, a damaged blob refused at load, a sidecar of a
   newer version listed and not loaded, two presets sharing one blob and
   cleanup keeping it, and a missing plugin's presets kept. Review: Opus
   (storage). **Parallel: yes.**
5. **Preset operations through the control server.** Files:
   `contracts/api` (`ModelAction` 9–12, `PluginPresetList`,
   `encode_load_preset`/`encode_save_preset`), `common/model`
   (`LoadInsertPreset`, `PresetRef`, `InsertMix.preset`, `modified`
   rules), `engine/control/src/engine_control_server.cpp` and
   `plugin_hosting.cpp` (steps 1–5 of §3, supersession, commit after
   `Loaded`), FakeEngine, conformance. Version: session format +1, protocol
   +1. Test first: `plugin_hosting_preset_load`. Covers a load committed
   only after `Loaded`, a refused state leaving the old host playing and
   the document unchanged, a superseded load answered Failed, the inverse
   restoring the prior state bit-exact, and a load during a take refused
   only when files would copy. Review: Opus (contracts). **Parallel: no.**
   It shares `engine_control_server.cpp` with Track 1, so it is sequenced
   after Track 1's in-flight slice merges, and the coordinator gives it
   that file's slot.
6. **The plug-in window's preset menu** (ui/qt). Pro Tools' Librarian menu:
   the name with "*" when modified; Previous/Next; Save Settings, Save
   Settings As, Rename, Delete; Factory, User and Sonora groups; "plugin
   missing" and "damaged" states; Undo Load Preset (Q1); Set as User
   Default (Q5). Key-mappable command ids `plugin.preset.next`/`.previous`
   in both keymaps. Screenshots of every scene touched under
   `docs/images`. Review: Sonnet (UI). **Parallel: yes** (ui/qt and keymaps
   only), after slice 5's protocol.
7. **Into the mix undo history**, when Track 2 item 6 lands.
   `LoadInsertPreset` makes one step, and the menu's Undo Load Preset is
   removed. Review: Opus (with item 6). **Parallel:** follows item 6.

Slices 1, 3 and 4 can run at once. 2 follows 1. 5 follows 2 and 4. 6
follows 5.
