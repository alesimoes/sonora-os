# Factory presets: what is read, and what does not survive

ADR-0029 §1, §5 (slice 3). The formats read here are the plugin standards'
own, published: LV2 `pset:Preset` (through lilv) and the VST3 SDK's
`.vstpreset` layout and `IUnitInfo` program lists. No DAW's preset format is
read, and none is written (ADR-0004, ADR-0029 Q3).

## Listing: the probe record, version 3

`sonora-plugin-host --probe` lists a usable plugin's factory presets in the
record's `presets` (contracts/plugin `probe_record.h`): `{id, name}` each,
at most 4096. The engine's catalog cache (version 2) keeps them. Content is
not read at listing.

| Kind | Id | Found by | Name |
|---|---|---|---|
| LV2 | `lv2:<preset URI>` | `lilv_plugin_get_related(plugin, pset:Preset)`: the plugin's bundle and any bundle on the LV2 path that declares a preset `lv2:appliesTo` it | `rdfs:label`, else the URI's tail; sorted by name, then id |
| VST3 program | `vst3-program:<unit id>/<index>` | the controller's `IUnitInfo`: each unit with a program list *and* a writable `kIsProgramChange` parameter of that unit | `getProgramName`, else "Program N" |
| VST3 file | `vst3-file:<path relative to its root>` | `*.vstpreset` under `<root>/<vendor>/<plugin name>/`, four folders deep, whose header names the plugin's class; roots `$HOME/.vst3/presets`, `/usr/share/vst3/presets`, `/usr/local/share/vst3/presets`, an earlier root shadowing a later one | the `Info` chunk's `<Attr id="Name">` (ADR-0144), else the file name without `.vstpreset` |

The `Info` chunk is untrusted XML, read through the header and the chunk
list alone (never the whole file) and scanned for the one attribute: at most
64 KiB, no DOCTYPE or declared entity (refused, never expanded), only the
five predefined entities and character references, strict UTF-8 without
control characters, the name cut to 128 bytes on a character boundary. No
`Info`, two of them, or any of these failing leaves the file name; it never
drops the preset.

## Reading: `--read-preset BUNDLE PRESET-ID --plugin ID`

One child per read, under the probe's timeout and crash rule (engine side:
`engine/control` `factory_preset_reader.h`). Its stdout is one line
(`PresetRead`) and then the raw StateBlob. Every kind is restored into a
real instance and run through validation before it is written, so a preset
the plugin refuses, or one that makes it write NaN or outside its buffers,
is refused there and never reaches a session.

- **LV2.** `lilv_state_new_from_world`; port values become `params` (port
  symbol, plain value; a value that is not a finite number refuses the
  preset); the rest is the state's Turtle, the StateBlob's one section,
  only when the plugin declares `state:interface`.
- **VST3 program.** The program-change parameter is set on the controller
  (`setParamNormalized`) and queued to the processor; four validation
  blocks run; then `getState` on the component and a separate
  controller. `params` are the controller's values afterwards.
- **VST3 file.** The `Comp` and `Cont` chunks are the StateBlob's two
  sections, byte for byte; the plugin must accept them (`setState`).
  `params` are the controller's values afterwards. A class id spelled in
  either byte order is the plugin's (a file written on Windows for a
  plugin bridged by yabridge may use COM order).

A VST3 program-change parameter is never among `params`: applied again on
top of a restored state at every load, it would put the factory program
back over the operator's edits.

## What does not survive

- **LV2 presets that name files.** A preset whose state holds an
  `atom:Path` (a sampler's sample) is serialised by lilv as it resolves
  it, an absolute path into the preset's bundle. It plays on this machine
  only. Making it portable is ADR-0029 §6 and slice 2 (`Plugin Files/`),
  not done here.
- **LV2 presets with `state:state` for a plugin without
  `state:interface`.** The properties are dropped; the port values are
  kept.
- **`.vstpreset` `Prog` chunks** are not read.
- **VST3 programs without a program-change parameter**, programs offered
  only through `IProgramListData`, and pitch names are not listed.
- **`.vstpreset` files inside a plugin's bundle**
  (`Contents/Resources/Presets`) or under any other folder than the three
  roots are not listed.
- **Byte-identical round trip of a VST3 program.** The state is whatever
  the plugin's `getState` returns after four blocks with the program
  selected; for a plugin whose state counts blocks or holds time it
  differs from read to read.

## Verified, and what remains

- Container (`tools/build-plugins.sh`, Debian trixie): the test plugins'
  presets, every refusal above, a crash and a hang (`plugins_host_read_preset`,
  `plugins_host_vstpreset_fuzz`, `factory_preset_reader`).
- A real LV2 preset that names a file, by hand in the container (2026-11-02):
  x42 zero-convolver's "No-OP Mono" (`zeroconvolv/pset#noopMono`) is listed,
  read, restored (the plugin loads its IR) and validated; its state names
  `file:///usr/lib/lv2/zeroconvo.lv2/ir/delta-48k.wav`, absolute, as
  predicted above.
- ADR-0029 Unverified 3, VST3 programs through a one-shot host: settled
  for the mechanism with the state test plugin (program 2 selected through
  its program-change parameter, four blocks, `getState` holds program 2's
  state). No VST3 in the container image exports a program: Steinberg's
  validator reports "does not export any programs" for the LSP set and for
  the SDK's mda ports (whose root unit names a program list id that no
  program list carries; the host lists nothing for it, correctly). Still to
  check with real plugins that do: Surge XT, and one plugin bridged by
  yabridge.
