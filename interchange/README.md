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
`approximated:` lines. Exit codes: 0 done, 1 failed, 2 usage. AAF and
`.ptx` are not here yet (they need libaaf and libptformat, a separate item).
Do not run it against a session an engine has open.

Tests: `ctest -R interchange`.
