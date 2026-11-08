# Context: plugin host

Local CI context: `sonora-plugins` (host) with engine tests. Agent: plugin-engineer.

## Where
- `plugins/host` (only target linking plugin SDKs), `engine/plugin_client`,
  `contracts/plugin` (shared with rt-engineer), `interchange/`.

## Bound by
- ADR-0012: one host per insert chain, depth 1, audio thread never waits;
  slots retired by epoch; a missed block is silence ramped in, 8 in a row
  remove the chain; timer-paced hosts by default (futex gate off).
- ADR-0017: Windows plugins via yabridge, bridged VST3 only (proposed).
- ADR-0029: presets and state files, LV2 file-bearing state.

## Build and test
- Labels: `pluginhost plugin_client`. `plugins/host` touches only these.
- Other `plugins/*` paths (not native/client/sdk/host) run the full suite.

## Stock set (ADR-0076)
- Manifest `plugins/content/stock.json` (schema 1, suite trixie, one entry per
  Debian binary package); package list
  `iso/config/package-lists/sonora-stock.list.chroot`.
- Check `tools/check-stock.sh`: manifest and list agree, spdx on the
  allow-list, categories in the six, brand deny-list.
- Probe `plugins/host/tests/test_stock_probe.cpp`, ctest
  `plugin_host_stock_probe` (label `plugins`).
- `SONORA_STOCK_OPTIONAL=1` lets the probe skip absent bundles, only in the PR
  micro build, never in release or nightly.

## Traps
- Host pacing tests are serial and fail under load. Wait on state, not time.
- Changing `contracts/plugin` is an amendment asked of its owner.
- Third-party plugin crashes must stay isolated in the host process.
- Stock scan granularity is the package: it lists every plugin of an installed
  package (lsp-plugins-lv2 brings many bundles), not only the ones the
  manifest names. Review the scan output before assuming one plugin appeared.
- `SONORA_STOCK_OPTIONAL=1` is a PR-only escape, never a way for a missing
  bundle to pass on a real image.
