# feature/plugin-host-lv2

- **Goal:** host side of plugin hosting, LV2 first (BACKLOG "Phase 4: plugin hosting").
- **State:** in review. Needs code-reviewer, and rt-engineer review of the draft contract.
- **Commits:** see `git log origin/main..feature/plugin-host-lv2`.

## Verify
    tools/build-plugins.sh -R plugin_host     # expected: 100% tests passed, 0 tests failed out of 11
    tools/build-engine.sh                     # expected: "plugins/host: lilv-0 not found ... skipped", 8/8 pass

## Done
- `sonora-plugin-host` (lilv): `--list [--json]`, `--probe URI`, `--shm-fd FD --chain ...` — plugins/host/src/main.cpp
- Draft contract, kLayoutVersion 0 — contracts/plugin/include/sonora/plugin/protocol.h; layout test `plugin_protocol`
- Futex wake behind a neutral seam — plugins/host/src/ipc.h, src/linux/ipc_linux.cpp
- Validation: canary guards, NaN check, channel mapping, required features — plugins/host/src/lv2_host.cpp
- Engine-role driver + test plugins (crash, sleep, liar) — plugins/host/tests/
- Figures and findings — brain/research/plugin-host-lv2-container.md

## Open
- Contract review by rt-engineer (ADR-0007 §6); the Phase 4 ADR (M9) decides pipeline depth and wake.
- Runtime canary check covers only 64 words past max_frames — lv2_host.cpp kRuntimeGuardWords.
- No state save/restore, worker, presets, editors, VST3.

## Do not redo
- Hand-built `file://` + relative path for bundles finds nothing; use lilv_new_file_uri on an absolute path (regression: plugin_host_probe_liar).
- Asserting zero over-period callbacks flakes in the container (one 5.9 ms VM stall per ~20 000); allowance is kOsNoiseCallbacks = 2.

## Pointers
- brain/decisions/ADR-0007 §4.3 and update 1 items 4 and 6; ADR-0010 §2; brain/specs/plugin-latency-compensation.md
