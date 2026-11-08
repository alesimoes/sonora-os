# N4 verification: mastering console and mastering pass

Status of BACKLOG `N4: mastering console + sonora-analyze mastering pass`:
the code is on main (ADR-0046; #436 processor, golden files, quality.json,
DPF target, analyze pass; #479 editor). This note records the run that
verifies it. No code, golden file or BACKLOG line was changed. The task
names ADR-0103 as the record of this decision, but no
`brain/decisions/ADR-0103*` exists in this checkout (main at 254e3219), so
the decision is recorded here and the ADR is reported as missing.

## What ran

Host: Windows 11 Enterprise 10.0.26300, Docker Desktop, image
`sonora-build:trixie` (Debian trixie, GCC, Release). Build directory: the
named volume `sonora-build-native.mastering.verify`, configured once from this
worktree (`cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Release`, configure
about 52 s). Jobs: 16 (`tools/build-slot.sh --jobs`), every build through
`tools/build-slot.sh`.

Build (narrowest targets the tests need):

```
cmake --build /build -j16 --target sonora-dsp-measure sonora-analyze test_analyze_mastering
...
[100%] Built target sonora-analyze
[100%] Built target test_analyze_mastering
exit=0
```

The first build attempt ran past the tool's 10-minute limit and its container
was stopped; the volume kept the objects, so the second attempt finished the
remaining targets. Both attempts are the same command.

Tests:

```
SONORA_DSP_QUICK=1 ctest --test-dir /build -R 'native_mastering|analyze_mastering|analyze_cli' --output-on-failure
    Start 272: native_mastering
1/6 Test #272: native_mastering .................   Passed   42.77 sec
    Start 566: analyze_mastering
2/6 Test #566: analyze_mastering ................   Passed    3.02 sec
    Start 569: analyze_cli_usage
3/6 Test #569: analyze_cli_usage ................   Passed    0.02 sec
    Start 570: analyze_cli_mastering_usage
4/6 Test #570: analyze_cli_mastering_usage ......   Passed    0.01 sec
    Start 574: analyze_cli_timbre_usage
5/6 Test #574: analyze_cli_timbre_usage .........   Passed    0.02 sec
    Start 575: analyze_cli_timbre_missing
6/6 Test #575: analyze_cli_timbre_missing .......   Passed    0.01 sec

100% tests passed, 0 tests failed out of 6
Total Test time (real) =  46.08 sec
exit=0
```

`native_mastering` is `sonora-dsp-measure --processor mastering` against
`plugins/native/mastering/quality.json` and its golden directory (label
`slow`, run by name as the task asked). `analyze_mastering` is
`test_analyze_mastering`. The `analyze_cli_*` tests drive `sonora-analyze`
through its usage and stub paths.

Not run here: `analyze_contract` and `analyze_tone_contract`, the RTSan
stage, the MSVC portable job, and the full suite. Those belong to the PR gate
and the nightly, not to this narrow check.

## ADR-0046 "Unverified" items, and what would settle each

ADR-0046 lists two open items. Two further gaps came out of this check. None
is closed by the run above.

1. **Target curve and thresholds are defaults, not tuned on a corpus.**
   Settled by: a corpus of reference masters (our own recordings, or
   licensed material recorded on a card, ADR-0011 §4.6) across genres, each
   measured for integrated LUFS, true peak, crest factor and spectral tilt;
   the suggestion pass run on each; the pass's chosen target compared with the
   measured target of the corpus's reference band; the agreement reported as a
   per-genre table. Done when the thresholds that the pass prints are inside
   the corpus's inter-quartile range. Tuning changes data only, not code.

2. **Auto-level is validated only on stationary noise** (target and
   ±12 dB clamp), not on programme with long silences or a changing section.
   Settled by: `sonora-dsp-measure` on the mastering processor with a
   programme-like stimulus (a music excerpt with silence gaps of 2 s and 10 s,
   and a section change of +6 dB at 20 s): the short-term loudness error after
   each change, the time to re-converge (within `autolevel.target_lu` = 0.5 LU),
   and the hold below -70 LUFS short-term (ADR-0046 says it holds its trim
   there rather than chase). Done when each case is a measured bar in
   `quality.json`.

3. **Rules are still code, not JSON** (gap found in this check; ADR-0046 does
   not list it). Settled by: the rule table moved to a data file, with the
   pass's output unchanged on the corpus of item 1 (byte-equal suggestion
   report before and after). Until then, the rules cannot be tuned without a
   rebuild.

4. **Console not yet on the Master insert** (gap found in this check; ADR-0046
   does not list it). The editor (#479) is built, but the console is not yet
   the insert on the Master bus. Settled by: an engine-level test that
   inserts the console on the Master bus and checks the bus output against the
   standalone processor with the same settings (the wiring test already covers
   the console against its parts, `wiring.max_abs`), plus the latency report
   checked through the engine's PDC. This needs the rt-engineer's and
   ui-engineer's work, not a change here.

## Result

All six selected tests pass on the current main code. The three pieces of
ADR-0046 in the "Unverified" section that are data or corpus work (items 1
and 2) and the two gaps found above (3 and 4) are open. The N4 BACKLOG line
should be closed only once items 1 and 2 have measured bars, and items 3 and 4
are either done or split out as their own BACKLOG lines.

Not measured here: no corpus, no stimulus of items 2 and 3, no CPU figure on
this host, no RTSan run. Each is listed as unverified.
