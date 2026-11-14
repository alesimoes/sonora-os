# .ptx import

Read-only. There is no `.ptx` export; to hand a session to Pro Tools use AAF
or stems. Decoding is libptformat (the published reverse-engineering result
that ships inside Ardour), behind the neutral `PtxReader` interface
(ADR-0156). It runs only in `sonora-convert`, never in the engine.

Build: `-DSONORA_WITH_LIBPTFORMAT=ON`. The library is found on the system
(`find_path`/`find_library`, then pkg-config `ptformat`); nothing is fetched.
With the option OFF, or the library missing, no `.ptx` reader is built.

## Kept
Audio sources (copied into the session folder; a missing file is an error
naming it), audio tracks, region positions, offsets and lengths, sample rate.

## Dropped (reported in the conversion report)
MIDI, plug-ins, automation, fades, inactive playlists, clip gain, markers,
routing. libptformat decodes Pro Tools 5 to 7 era files; others fail loudly.

## Fallback
Export AAF from Pro Tools and import that (`interchange.sessions.aaf_import`).
