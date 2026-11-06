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

## Traps
- Host pacing tests are serial and fail under load. Wait on state, not time.
- Changing `contracts/plugin` is an amendment asked of its owner.
- Third-party plugin crashes must stay isolated in the host process.
