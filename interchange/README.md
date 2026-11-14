# interchange and sonora-convert (ADR-0007 M10)

Offline conversion between other formats and the native session. It is not
linked by the engine and nothing in it runs on the audio thread: a hostile
file can crash a converter, never a session.

    sonora-convert smf-import <in.mid> <out.sonora>
    sonora-convert smf-export <in.sonora> <out.mid>
    sonora-convert session    <in.sonora> <out.sonora>

- `smf-import`: Standard MIDI File format 0 or 1 to a new session. Each file
  track with notes or events is a MIDI track with one region at tick 0; tempo
  and time-signature events become the tempo map at 48 kHz. Markers, key
  signatures, lyrics and other meta events are dropped.
- `smf-export`: MIDI and instrument tracks (active playlist, unmuted regions)
  to format 1 at 960 ppq with a conductor track. Audio, markers, mix and
  plugins are dropped; positions are rounded to the 960 grid.
- `session`: a session of any older format version rewritten in the current
  one, through the engine's own saver.

What did not survive is printed to standard output as `dropped:` and
`approximated:` lines. Exit codes: 0 done, 1 failed, 2 usage. `.ptx`
is not here yet (it needs libptformat, a separate item).
Do not run it against a session an engine has open.

Tests: `ctest -R interchange`.

## Import Session Data (ADR-0123)

Tracks taken from another `.sonora` session into the open one live in
`common/session_files` (`session_import.h`, `session_import_copy.h`), not
here: the planner returns one undoable `Compound` and the list of audio
files to bring into `Audio Files/` (hard link, else a synced copy, all
deleted again on failure or cancel). Audio tracks with their active
playlist, mix (never solo), inserts without plug-in state, and volume and
pan lanes come over; MIDI and instrument tracks, sends, buses, groups,
VCAs and folders are listed as `dropped:`. A newer format, a missing file,
another sample rate or an open take refuses the import before anything is
written. Logic projects come in through AAF or stems, not through this.
Tests: `ctest -R session_import`.

## AAF import (ADR-0154)

`sonora-convert import x.aaf out.sonora` (or `aaf-import`) reads the
composition through libaaf (LGPL-2.1-or-later), maps it with the neutral
`AafDocument` and copies the essence it references into `Audio Files/` as BWF
at the session rate. The file is looked for at the path the mapper chose,
beside the `.aaf`, in `Audio Files/` and in `Media/`; a missing one is an error
naming it. CMake option `SONORA_WITH_LIBAAF` (default OFF) compiles the
adapter; the library is found with `find_path`/`find_library`, never fetched.
With it OFF the command exits 1 with "built without AAF support". Tests with
the library: `ctest -R aaf_libaaf`. Logic projects come through AAF or stems.

## OMF import (ADR-0160)

`sonora-convert import x.omf out.sonora` (or `omf-import`) reads an OMF
composition with our own parser (`omf/`), extracts embedded media beside the
output and copies it into `Audio Files/`; media referenced by path is looked
up beside the `.omf`, in `Audio Files/` and in `Media/`. A truncated or
corrupt file exits 1 with a message. Import only, no OMF export; tested on
synthetic files, no real-world OMF verified. Tests: `ctest -R omf_`.
