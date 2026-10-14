# feature/dsp-n2-dynamics

- **Goal:** ADR-0011 N2 slice 6a, the optical levelling amplifier "Sonora Opto" (`planning/BACKLOG.md`, N2 slice 6a; 6b FET is out of scope).
- **State:** in review (code-reviewer, rt-safety-auditor). Not merged.
- **Commits:** see `git log origin/main..feature/dsp-n2-dynamics`; one commit on main 4be3baf.

## Verify
    tools/build-plugins.sh > build-plugins.log 2>&1; grep "tests passed" build-plugins.log   # expected: 100% tests passed, 0 tests failed out of 488

## Done
- common/dsp opto.h cell, attenuator, side chain: ctest dsp_opto
- SPICE reference: ctest spice_opto_0..5, plugins/native/opto/tests/spice/
- plugins/native/opto meets quality.json: ctest native_opto (204 held checks, 0 failing; build-plugins/dsp-reports/opto.json)
- Bundles: lv2lint 2/2, VST3 validator 47/47 x 2, 22 host renders bit-exact, control_state x 2
- CPU: eco 8.7-9.1 us p99.9 (class A held, calibrated too); standard 42-47, high 81-93 us reported (tools/dsp-cpu.sh build-plugins 60 opto)

## Open
- Standard (4x) over class A: optimisation in BACKLOG "N2 opto follow-ups" (a)
- Unverified on hardware: limit/compress switch reading, buffered side-chain tap (research note)
- Pre-existing: tube hot_input wall-clock bar under ASan on the laptop (BACKLOG "Wall-clock bars under the sanitizers")
- RTSan and MSVC: CI only (rtsan job); not run locally

## Do not redo
- 2x as the default quality: -89.2 dBc aliasing at 44.1 kHz, over the -90 bar
- Feed-forward key: failed native_format_nokey_vst3_opto (key must go through the attenuator share)
- Backward-Euler cell with light at step start: 0.13-0.26 dB lag vs ngspice

## Pointers
- brain/research/native-opto-n2.md
