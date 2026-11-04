# Agent queue (V1)

The coordinator launches the next line the moment an agent reports, so that at
most 10 work and none sits idle. Order = value. V1 is recording, improving
the mix, improving vocals, and analysing and correcting the mix for analog
recording; MIDI, virtual instruments, video and immersive are out. Each launch
is a whole task (contract, engine, UI, tests, docs), one PR, one row; items that
touch the same files share a row. Contracts go first and merge first. The
coordinator reserves ADR numbers and the protocol version at launch.
`tools/orchestrator.py import-queue planning/AGENT-QUEUE.md` reads this table.

Already running or in an open PR on 2026-11-04, so not listed: comping (lanes,
exit test, loop takes, consolidate #367), automation write and list (#385),
tempo map and edit, session management, clip groups and sample edit, freeze and
offline plugins, edit window open parts, audio export, mixassist reference
analysis, vari-mu compressor, cab IR trim, portability fs and process, ISO
build (#398), tuner (#383), mastering (#384), vocal balance (#391), mix
planners (#396), tremolo (#397), transformer and passive EQ (#382), RF64
(#380), bounce (#378), import (#387), loudness panel (#388), punch and loop
ranges (#389), VCA and folders (#401), mix/edit groups (#400), preferences
first slice (#390).

| Order | Item | Agent | Waits on |
|---|---|---|---|
| 1 | ui.mixer.sidechain, ui.mixer.io (sidechain routing UI and model; I/O setup window) | ui-engineer | #400, #401 merged |
| 2 | ui.mixer.pan (stereo pan, stereo tracks in the model) | ui-engineer | #400 merged (session format) |
| 3 | ui.editing.waveform (peak-file cache, real overviews) | ui-engineer | edit window open parts |
| 4 | ui.editing.clip_gain (UI and render on the #400 model) | ui-engineer | #400 merged |
| 5 | time.offline_tce (offline time compression/expansion and pitch shift) | dsp-engineer | freeze and offline plugins (offline render path) |
| 6 | plugins.graph.parallel (parallel plugin graph) | plugin-engineer | freeze and offline plugins (same graph files) |
| 7 | plugins.host.oop, plugins.host.scan_worker, plugins.host.state (control-server wiring, scan worker check, #276 follow-ups) | plugin-engineer | none |
| 8 | plugins.host.manager, plugins.host.editors (manager UI, categories, favourites, editor embed, LV2 editors) | plugin-engineer | row 7 |
| 9 | native.ai_analysis (ONNX runtime, models, key and timbre in sonora-analyze) | dsp-engineer | #384 and #399 merged |
| 10 | mixassist.plans (engine and UI diff preview wiring for plans) | rt-engineer | #396 merged |
| 11 | ui.workflow.windows, ui.workflow.preferences (window configs, track heights; I/O, buffer, record paths) | ui-engineer | #390 merged |
| 12 | ui.mixer.metering, time.varispeed (UI strips on post-fader data; varispeed control) | ui-engineer | #388 merged |
| 13 | plugins.stock (remaining stock set) | dsp-engineer | #397 merged |
| 14 | ampcab.captures (captures and cards) | dsp-engineer | owner capture hardware call |
| 15 | ui.keymaps.tty1 (console chords) | ui-engineer | #398 merged |
| 16 | native.analog, native.editors (remaining analog models, telemetry region in the generic editor) | dsp-engineer | #382 merged |
| 17 | ampcab.artist_tone | dsp-engineer | native.ai_analysis (row 9) |
| 18 | architecture.adr0007.m1_docs, architecture.portability.charters, architecture.portability.ci_qt_windows | rt-engineer | #404 merged |

Hardware-gated (no interface attached, owner step): drivers.umc1820.*,
system.usb.physical, system.boot_tests.hardware, system.hardening.soak,
plugins.graph.benchmark, plugins.formats.yabridge (real run).
