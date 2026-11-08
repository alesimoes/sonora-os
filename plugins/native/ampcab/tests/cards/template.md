# <capture id>

The unfilled card template: every field is a placeholder, so the checker
must refuse it. Copy it to brain/models/<id>.md and fill every field.

```card-json
{
  "id": "<id>",
  "name": "<name the sound, no brand>",
  "kind": "capture",
  "source": "hardware",
  "unit": {"type": "", "serial": "", "ownership": "owned"},
  "settings": {},
  "rig": "",
  "date": "YYYY-MM-DD",
  "third_party": null,
  "capture": {"architecture": "", "sample_rate": 0, "train_commit": ""},
  "file": {"name": "<id>.nam", "sha256": "", "size": 0}
}
```
