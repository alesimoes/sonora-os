"""Title and body of the one PR the daemon opens for a feature, from the
integrator's result (`ready:<feature>` meta) and the reservations."""
import json


def pr_text(store, f, ts, attribution):
    try:
        ready = json.loads(store.get_meta(f"ready:{f['id']}", "{}"))
    except ValueError:
        ready = {}
    summary = (ready.get("summary") or "").strip() or f["id"]
    title = f"{f['id']}: {summary.splitlines()[0]}"[:100]
    if f["partial"]:
        title = "[partial] " + title
    ids = {x["id"] for x in ts}
    nums = [f"{r['kind']} {r['number']}" for r in store.reservations()
            if r["task_id"] in ids]
    lines = ["## Summary", summary, "", "## Tasks"]
    lines += [f"- {x['id']} ({x['agent']})" for x in ts]
    if nums:
        lines += ["", "## Reserved numbers", ", ".join(nums)]
    for head, key in (("Follow-ups", "followups"),
                      ("Unverified", "unverified")):
        items = ready.get(key) or []
        if items:
            lines += ["", f"## {head}"] + [f"- {i}" for i in items]
    lines += ["", attribution]
    return title, "\n".join(lines)
