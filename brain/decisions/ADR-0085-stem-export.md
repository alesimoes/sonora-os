# ADR-0085: Stem export as offline solo-in-place bounces

Status: Accepted (recommended defaults, 2026-11-08; BACKLOG `interchange.stems`).
Builds on: ADR-0007 §4.1 and §6 (the control API and the contract lock), Bounce Mix (`contracts/api/include/sonora/api/bounce.h`, protocols 49 and 61).
Scope: the contract. `contracts/api/include/sonora/api/stems.h` and its test. The engine render (`engine/control` stems plan, job and command) is the second slice of the same feature; the File > Export Stems window is the follow-up feature `interchange.stems.ui`.

## Summary

`mix.stems` writes one file per track, or one per output (each bus and the
master), into `Bounced Files/<name> Stems/`. Its argument is the bounce
argument with two words in front, `by=track|output` and
`tracks=<id>,<id>...`; everything else, the name last, is parsed by
`parse_bounce_argument`, so a stem file has every option a bounce has.
Stems render offline only; `mode=realtime` is refused with a reason. The
engine runs the existing offline bounce once per stem on a copy of the mix
snapshot whose solo flags select that stem. Progress, the result and a
refusal ride the existing `EventKind::Bounce`; there is no new event kind and
no protocol bump. The capability `mix.stems` in `Hello::capabilities` says an
engine has it.

## Context

Stems are how a session leaves SonoraOS for a DAW that cannot read our
project: mastering, mixing elsewhere, and Logic, which has no published
project format (ADR-0004; we do not claim `.logicx` import and interchange
with Logic goes through AAF or stems). A stem set is only useful when every
file starts at the same frame and has the same length, so that the files,
dropped at bar 1, line up and sum to the mix.

Bounce Mix already renders the whole mix offline to one file with the
format, rate, dither and loudness choices the operator expects. What is new
is choosing which tracks each pass hears, and writing many files as one
result.

## Decision

1. **Command and argument.** `mix.stems`, argument
   `by=track|output [tracks=<id>,...] <mix.bounce argument>`.
   `by` defaults to `track`; an absent or empty `tracks=` is every unmuted
   audio track. `name=` is last and holds the rest of the field (spaces,
   `;` and `=` included); a `by=` or `tracks=` after it belongs to the name.
2. **Refusals, each with a reason.** An unknown `by`; a repeated `by=` or
   `tracks=`; a track list with an empty item, a non-number, `0`, more than
   18 digits or a repeated id; `tracks=` with `by=output`; `mode=realtime`;
   and every refusal of `parse_bounce_argument`, its `mix.bounce:` prefix
   rewritten to `mix.stems:` so the operator sees the command they sent.
3. **Render.** One offline bounce per stem, on a copy of the mix snapshot
   whose solo flags select that stem's tracks (solo in place: the stem keeps
   its pan, inserts, sends and bus processing). The document is never
   touched. All files share the range `[start, end)`.
4. **Files.** `Bounced Files/<name> Stems/<NN> <track or output>.<ext>`.
   `stems_folder(name)` is `kStemsParent + name + kStemsFolderSuffix`.
   A failed or cancelled export removes the whole folder: a partial stem
   set is worse than none, because it looks complete.
5. **Event.** `EventKind::Bounce`, with `frame` the thousandths of the whole
   export over every file, `insert` the number of files once Done, `flags`
   the `BounceState`, `text` the folder relative to the session once Done
   and the reason once Failed or Cancelled. Done, Failed and Cancelled are
   held notices, as for a bounce.
6. **One export at a time.** A bounce and a stem export share the rule, and
   `mix.bounce_cancel` cancels either.
7. **No protocol bump.** The command is new and optional; the capability is
   the discovery. The event's fields keep the meaning bounce.h gives them,
   read per command.

## Rationale

- Reusing the bounce argument means every format, dither and normalise rule
  and its tests apply to stems at no cost, and a fix to one is a fix to both.
- Solo in place on a snapshot copy is the only way the stems sum to the mix:
  the full chain, bus compression aside, is what each stem goes through. The
  null test of the engine slice (sum of the stems equals the full bounce
  within 1 LSB at 24 bit, with no bus processing that is level-dependent)
  holds the engine to it.
- Offline only: a real-time pass per stem costs the session's length n times
  over and gives nothing the offline pass does not; outboard gear in an
  insert is the one reason for real time, and it is out of scope here.
- The existing event already carries a fraction, a count and a text, which
  is all a stem export reports; a new event kind would be a protocol bump and
  a second display path for the same notice.

## Consequences

- `stems.h` includes `bounce.h`; a breaking change to the bounce argument is
  a breaking change to stems, and the stems test fails with it.
- A client must remember which command it sent to read a `Bounce` event as a
  file path or as a folder. Two exports never run at once, so there is no
  ambiguity on the wire.
- Bus processing that depends on level (a bus compressor) makes the stems sum
  differently from the mix; this is inherent to stems, not to this design,
  and the export window should say so.
- `mix.stems` is not yet in the command vocabulary of
  `contracts/api/include/sonora/command.h`; the engine slice adds it with
  the command it registers.

## Alternatives rejected

- **A new `EventKind::Stems`.** A protocol bump (none is reserved) and a
  second notice path in the UI for the same three outcomes.
- **Extending `BounceOptions` in `bounce.h`.** Every bounce client and test
  would see stem fields it never uses, and the contract change would be
  breaking for a header that is already stable.
- **A separate argument grammar for stems.** Two parsers for one set of
  formats drift; the bounce parser is the one with the tests.
- **Muting the other tracks in the document for each pass.** It touches the
  operator's session, needs an undo story, and a crash mid-export leaves the
  session muted.
- **Real-time stems.** Refused, see Rationale.
- **Rendering from each track's pre-fader signal (raw track stems).** Does
  not sum to the mix; it is a different product (track freeze or consolidate)
  and can come later as `by=` value of its own.
- **One interleaved multichannel file.** Few DAWs import it with names and
  the operator cannot audition one stem; one file per stem is what every
  mastering engineer expects.

## Unverified

- The planning document `planning/plans/interchange.stems.md` was on no
  branch when this ADR was written; it follows the plan's task brief in the
  orchestrator's plan record. The architect is asked to reconcile any
  difference.
- The engine slice is not written: the null test, the folder removal on
  failure and cancel, and the per-stem naming are decided here and
  unverified until it lands.
- That the stems open lined up in other DAWs (a BWF's time reference is the
  start; a plain WAV, AIFF or FLAC relies on the operator dropping them at
  the same position) has not been checked against any other DAW.
- The by=output grouping for a track that feeds the master and a bus by a
  send: it appears in its output's stem only; a send's bus stem carries the
  send's share. This is the engine slice's to test.
