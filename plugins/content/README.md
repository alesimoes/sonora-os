# plugins/content: the stock effects set

The image ships a curated set of free LV2 effects from Debian's archive
(ADR-0076). The set exists so the DAW has an EQ, dynamics, reverb and delay
before the native suite (`plugins/native/`, ADR-0011) passes its quality bar.
Each package names the native processor that retires it.

Nothing here is vendored or built by us. The packages are installed by
`live-build` from the archive pinned by `SONORA_SNAPSHOT` in
`iso/auto/config`. They land in `/usr/lib/lv2`, which is on lilv's default
path, so `sonora-plugin-host` finds them and probes them like any
third-party plugin (ADR-0012).

## `stock.json`

Top level:

| Field | Type | Meaning |
|---|---|---|
| `schema` | integer | The schema version. It is `1` now and goes up when a field changes meaning. |
| `adr` | string | The decision this file records: `"ADR-0076"`. |
| `suite` | string | The Debian suite the image is built from (`"trixie"`). |
| `lv2_path` | string | Where the packages install their bundles (`"/usr/lib/lv2"`). |
| `spdx_allow` | string[] | The licence allow-list (ADR-0076 Decision 5). It is changed only by an ADR amendment. |
| `packages` | object[] | One entry per Debian binary package (below). |

Each entry in `packages`:

| Field | Type | Meaning |
|---|---|---|
| `package` | string | The Debian binary package name, exactly as `apt` takes it. |
| `version_pin_policy` | string | Only `"snapshot"` exists: the version is whatever `SONORA_SNAPSHOT` carries. Bumping the timestamp moves it. The installed system then follows the suite's updates. |
| `spdx` | string | An SPDX expression for the **code** in the binary package, read from its `debian/copyright`. It may use only `AND`, and only ids in `spdx_allow`. |
| `category` | string[] | One or more of `eq`, `dynamics`, `reverb`, `delay`, `modulation`, `saturation`. |
| `replaced_by` | string or null | The `plugins/native/<name>` processor whose arrival starts the package's retirement. `null` means no successor is planned. |
| `retire_when` | string | The condition, in words. It names every native processor that must pass ADR-0011 §3 first. |

Rules:

- The unit is the package. Every plugin a listed package installs appears in
  the browser. Hiding single plugin URIs is a deferred follow-up (ADR-0076,
  Alternatives rejected).
- Never list `GPL-2.0-only`, anything NonCommercial or no-derivatives, or any
  licence not in `spdx_allow`. If any code file of a package carries one,
  the package stays out.
- No package whose plugin names, descriptions or presets name a commercial
  product, and no Apple or Avid content (ADR-0004, ADR-0011 §5).
- Adding a package: first read its `debian/copyright` and confirm it exists
  at `SONORA_SNAPSHOT`. Then change the manifest and
  `iso/config/package-lists/sonora.list.chroot` in one PR.
- Retiring a package: remove it from both files in one PR.

## Non-code assets

These are not in `spdx`. They are recorded here so the licence record is
complete. They are unverified until the researcher confirms what each
binary package actually installs (ADR-0076, Unverified).

| Package | Asset | Licence |
|---|---|---|
| `zam-plugins` | editor artwork (SVG) | CC-BY (version not stated in `debian/copyright`) |
| `dragonfly-reverb-lv2` | fonts | OFL-1.1, Bitstream Vera |

## Held out

- `calf-plugins`: its stable `debian/copyright` reads `LGPL-2+`
  (LGPL-2.0-or-later), which is not on the allow-list as written. It
  enters only after an ADR-0076 amendment (ADR-0076 Decision 7).

## Check

Run from the repository root:

```sh
python -m json.tool plugins/content/stock.json > /dev/null
python - <<'EOF'
import json
m = json.load(open("plugins/content/stock.json"))
allow = set(m["spdx_allow"])
cats = {"eq", "dynamics", "reverb", "delay", "modulation", "saturation"}
for p in m["packages"]:
    ids = [t for t in p["spdx"].split(" AND ")]
    bad = [t for t in ids if t not in allow]
    assert not bad, (p["package"], bad)
    assert set(p["category"]) <= cats and p["category"], p["package"]
    assert p["version_pin_policy"] == "snapshot", p["package"]
print("ok", len(m["packages"]))
EOF
```
