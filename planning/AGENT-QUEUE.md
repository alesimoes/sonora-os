# Agent queue (V1)

The coordinator launches the next line the moment an agent reports, so that at
most 10 work and none sits idle. Order = priority. V1 is recording, improving
the mix, improving vocals, and analysing and correcting the mix; MIDI, virtual
instruments, video and immersive are out. Each launch is a whole task (contract,
engine, UI, tests, docs), one PR; contracts go first and merge first. The
coordinator reserves ADR numbers and the protocol version at launch.

| Order | Item | Agent | Waits on |
|---|---|---|---|
| 1 | plugins.stock (EQ, dynamics, reverb, delay, modulation, saturation) | dsp-engineer | CMake discovery refactor |
| 2 | native.tuner (vocal tuner) | dsp-engineer | CMake discovery refactor |
| 3 | native.vocal_balance (resonance suppression, analysis) | dsp-engineer | mixassist.plans contract |
| 4 | native.mastering (console + suggestion pass) | dsp-engineer | mixassist.plans contract |
| 5 | mixassist.workflow (gain staging, buses, parallel compression) | architect, then rt-engineer | mixassist.plans contract |
| 6 | ui.mixer.groups, ui.mixer.vca, ui.mixer.folders | ui-engineer | consolidate control command (protocol 43) |
| 7 | ui.mixer.io, ui.mixer.sidechain | ui-engineer | ui.mixer.groups |
| 8 | ui.record.punch_loop, ui.comping.lanes, ui.comping.exit_test | ui-engineer | consolidate control command |
| 9 | automation.list finished from #361, automation.write open parts | rt-engineer | build throttle merged |
| 10 | plugins.graph.parallel, plugins.graph.freeze, plugins.offline | plugin-engineer | plugin integration contract |
| 11 | native.ai_analysis (ONNX in sonora-analyze) | dsp-engineer | mixassist.plans contract |
| 12 | installer slice b+c (session carry-over with SHA-256, tty2 front end) | distro-engineer | #353 merged |

Hardware-gated (no interface attached, owner step): drivers.umc1820.*,
system.usb.physical, system.boot_tests.hardware, system.hardening.soak.
