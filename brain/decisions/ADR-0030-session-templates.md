# ADR-0030 — Session templates: a session file without material, kept in a per-user templates folder

- Status: Accepted (recommended defaults, taken by the Track 2 agent with no
  owner available, 2026-11-02)
- Date: 2026-11-02
- Builds on: ADR-0018 §5 and its update of 2026-10-27 (Save As, Save Copy
  In), ADR-0012 §6.1 (plugin state files), session-format.md "Versioning"
- Progress item: `daw.model.session_mgmt` (the templates part)

## Summary

*Navigation only; the sections below are the decision.*

- §1: a template is a **session file of the current format** in a folder of
  its own, `<templates>/<Name>/<Name>.sonora`, with the `Plugin State/` files
  it names. It has no key and no version of its own. **No session format
  bump, no protocol bump.** What makes it a template is the folder it is in.
- §2: what a template keeps and drops: the setup (tracks, buses, routing,
  inserts with their values and state, automation, tempo map, transport,
  markers on request), none of the material (regions, sources, left takes,
  the session id). `model::template_of()`.
- §3: Save as Template is the engine's **Save Copy In without audio** of the
  template's bytes into the templates folder. New from Template is **Open
  of the template's folder with the new session's document, then Save
  As** into the sessions folder. Both are messages the engine already takes.
- §4: the templates folder is per user: `$SONORA_TEMPLATES_DIR`, else
  `$XDG_DATA_HOME/sonora/Session Templates` (`~/.local/share/...`).
- §5: a template is read with the same strict loader and version checks as
  a session; a newer one is refused, an older one is migrated in memory.

## Context

The progress item `daw.model.session_mgmt` lists "Save As, Save Copy,
templates, autosave and rolling backups". #164 landed everything but
templates. Pro Tools' session templates (`.ptxt`, File > Save as Template,
the Dashboard's "Create from Template") and Logic's project templates keep a
session's tracks, routing and plug-ins without its audio, so an engineer
starts every tracking day from the same setup.

What exists:

- `session_files::SessionFolder::save_as()` makes a new, self-contained
  session folder from a source folder: media linked or copied (or left out
  for Save Copy In), then the sequence files and the plugin states the bytes
  name, copied from the source folder, then the session file, last. A
  failure takes back what it made.
- `ModelAction::SaveAs` (protocol 29) carries the target path, the flags
  (`kSaveAsCopy`, `kSaveAsNoAudio`) and **the bytes the interface made**; the
  engine's saver writes those bytes, with the engine's active folder as the
  source of the plugin states.
- `ModelAction::Load` carries a path (ADR-0018 §2) the engine enters before
  the document swaps, and does not compare the bytes with the file.
- Plugin hosting reads each insert's state from the **active** session
  folder when it starts a chain (`plugin_hosting.cpp`).
- The session format is strict: an unknown key is an error, and "adding a
  key with a default is still a version bump" (session-format.md,
  "Versioning").

## Decision

### 1. A template is a session file; the folder is the flag

A template is `<templates>/<Name>/<Name>.sonora`, a session file of
`model::kFormatVersion`, and beside it the `Plugin State/<sha256>.state`
files it names. It holds no `Audio Files/` content, no `MIDI Files/` and no
`MIDI Sequences/` (it has no region to name one).

There is **no `"template"` key**. A key would be a format bump (15) by the
format's own rule, and every build of version 14 would refuse every
template with "unknown key", for a flag that tells the loader nothing it
needs: a template opens exactly as a session does. The flag is where the
file is. The interface lists `<templates>` for New from Template and never
lists it for Open, and the sessions folder is never listed as templates.

A session file copied into the templates folder by hand is a template too;
§3 strips its material when a session is made from it.

### 2. What a template keeps and what it drops

`model::template_of(session, {markers})` (common/model,
`session_template.h`), with no I/O:

| Kept | Dropped |
|---|---|
| every track, in order, with its kind, channels, name and id | every region and crossfade, audio and MIDI |
| each track's **active** playlist, empty, with its id and name | every other playlist (they hold takes, and empty they are noise) |
| inserts: plugin, slot, active; their values, bypass and state reference | every source in the pool, audio and MIDI |
| sends, outputs, aux buses and their values; the monitor bus | `left_takes` |
| the whole mix: faders, pans, mute, solo, solo safe, inputs, monitor modes, MIDI routing | `session_id` (a template is no session) |
| automation lanes and their modes | |
| tempo map, meters, timecode, transport (loop, punch, rolls, count-off), sample rate, audio format, name | |
| markers, **only when the operator asks** (`TemplateOptions::markers`, default off) | markers otherwise |

Recommended defaults taken:

- **Automation is kept.** It is mix, and Pro Tools' templates keep it.
- **Markers are off by default**, offered as a check box: a song form is
  worth keeping, a session's cue points usually are not.
- **Only the active playlist is kept**, empty: alternates are takes.
- **Ids are kept** and `next_id` with them. They are unique within the
  file, which is all the format asks; a session made from the template
  starts its id counter past them.
- An insert whose plugin is **not installed** is kept with its values, as
  the session file keeps it; a session made from the template treats it as
  any session does (ADR-0012).

### 3. Save as Template and New from Template use the existing messages

**Save as Template** (File > Save as Template…): the interface flushes the
plugins' state as for Save, builds `template_of(current)` named after the
template, serialises it and sends `ModelAction::SaveAs` to
`<templates>/<Name>/<Name>.sonora` with `kSaveAsCopy | kSaveAsNoAudio`. The
engine's saver copies the plugin states the bytes name from the active
folder, writes the file last, and the session stays where it is. Refused,
as Save Copy In is, while a take is open, for a name that cannot name a
folder, and for a template name already in use.

**New from Template** (the template browser beside Open): the interface
reads the template file with `read_session_file()` (the strict loader),
refuses one at another sample rate as Open does, builds
`session_from_template(template, name, new_session_id())`, then:

1. `Load` with **the template file's path** and the new session's
   document: the engine enters the template's folder, so the plugin chains
   start from the template's states;
2. as soon as the engine has accepted the Load, `SaveAs` with flags 0 to
   `<sessions>/<Name>/<Name>.sonora`:
   the engine links or copies the template folder's media (there is none),
   copies its plugin states, writes the new file and moves the session
   there. The window names the new session once the engine's
   `SessionFolder` event says it moved, as for any Save As.

The name is checked against the sessions folder before the Load, so the
Save As is refused only by the medium. The Save As waits for the Load's
answer rather than following it on the socket: after a refused Load the
engine keeps the old session, and a Save As then would copy that
session's audio under the new name. A refused or lost Load drops it, and
so does any later Open.

Why not an empty-path Load (a new Untitled folder): the engine would start
the chains in a folder without the template's states, and every insert
would open with its state lost. Why not a new engine action
(`NewFromTemplate`, protocol 30): it is the clean form (the engine would
make the new folder first and never enter the template's), but it changes
`engine_control_server.cpp` and the wire, which Track 1 holds this round.
It is kept as the follow-up if §3's window (below) is ever seen to matter.

The window between 1 and 2: the engine is in the template's folder for one
round trip to the interface. A take started in it would land in the
template's `Audio Files/`; the engine refuses the Save As while a take is
open, and the window would then say so, held. Record during that round
trip needs a key pressed within a tick of New from Template. If the Save
As fails, the window keeps the new session unsaved, a held notice says
why, and its first Save or Save As goes to the sessions folder, never
beside the template (`SessionModel::saveAsRoot()`). An engine autosave in
that state would go to the template's `Session File Backups/`, never over
the template file. The `NewFromTemplate` action above closes all of this.

### 4. Where templates live

Per user, outside the sessions folder so Open never lists them and the
engine's untitled-folder cleanup never sees them:

1. `$SONORA_TEMPLATES_DIR`, when set (tests, and an operator who wants them
   on another volume);
2. else `QStandardPaths::GenericDataLocation` + `/sonora/Session Templates`:
   `$XDG_DATA_HOME/sonora/Session Templates`, by default
   `~/.local/share/sonora/Session Templates`. `/home` is what the
   persistence partition keeps (ADR-0005), so templates survive a reboot of
   the live stick and an image update.

The layout is the sessions folder's (`<Name>/<Name>.sonora`), so the
interface lists templates with the same `list_sessions()`. Categories
(Pro Tools' template groups, one subfolder each) are a follow-up.

### 5. Versions and checks

A template is read by `model::deserialize()`: strict keys, invariants, a
newer `sonora_session` refused naming both versions, an older one migrated
in memory with its repairs shown. A template written by an older build is
migrated when a session is made from it; the template file itself is
rewritten only by a new Save as Template over it, which Pro Tools also
requires. `file_format_version()` reads a template's version without
loading it, for a browser that wants to grey out a newer one.

## Rationale

- Reusing the session file means one reader, one writer, one set of
  fixtures and one migration path. A template format of its own would need
  all four, and would drift.
- Reusing `SaveAs` means the template folder is written by the one writer
  in the product (the engine's saver), durably, with the same take-back on
  failure and the same plugin-state copy that #131 and #164 fixed.
- The interface stays read-only on disk (ui/qt `session_files.h`).

## Consequences

- No session format bump, no protocol bump, no engine change.
- `common/model` gains `session_template.h` (`template_of`,
  `session_from_template`) and its test; `common/session_files` gains a test
  that drives both steps through the real `SessionFolder`.
- ui/qt `SessionModel` gains `saveSessionAsTemplate()`,
  `newSessionFromTemplate()` and `templateList()`; the menu entries, the
  name dialog with the markers check box, and the template browser beside
  Open are a ui-engineer item, with screenshots.
- A template whose state file was deleted refuses New from Template by
  name, and no half-made session folder is left (save()'s rule).
- Found on the way: through the real wiring (`ControlAdapter`, replica), a
  Save As reply and the `SessionFolder` event announced just before it
  arrive in one poll, and the reply was handled first, so the window took
  every moved Save As for "saved as a copy only". `ControlAdapter` now hands
  `SessionFolder` to the session before the model replies
  (`setFolderSink`); #164's tests ran behind a `ModelDocument` and could not
  see it. The FakeEngine test of New from Template is its regression test.

## Alternatives rejected

- **A `"template": true` key.** A format bump for nothing the loader needs
  (§1).
- **A `.sonoratemplate` single file** with the states inline or zipped: a
  new reader and writer, and plugin states that run to megabytes inside the
  document, which ADR-0012 §6.1 moved out of it.
- **Templates under the sessions folder** (`~/Sessions/Templates/`): Open
  and the unsaved-recordings scan would have to learn to skip it, and a
  session named "Templates" would collide.
- **New from Template as an empty-path Load**: the plugin states would be
  lost (§3).
- **A `NewFromTemplate` engine action now**: the clean form, deferred only
  because of who holds the files this round (§3).
