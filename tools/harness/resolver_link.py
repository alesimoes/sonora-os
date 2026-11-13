"""A stall resolver's PR carries its feature (2026-11-13).

A stall resolver that finds a feature's slices finished but never integrated
merges them itself on a branch of its own (`fix/comping-lanes-691`) and opens
the PR. The adoption sweep takes that PR as a train entry `pr-N`, which
merges it, but nothing tied the entry back to the feature: feature_pr_link
and feature_pr_merged only know the feature's own branch. #687
(ui.comping.lanes) and #688 (interchange.sessions.aaf_export) merged that
way and their features sat in `building` with stall alerts for hours.

The resolver says which branch it pushed: `branch` (and optionally `pr`) in
its result.json. Each sweep reads the results of finished resolver runs it
has not read yet and records branch -> feature in the store. For a feature
a resolver was queued for, a PR whose title starts with the feature id
(`<fid>: ...`, the resolver brief asks for it) is that feature's too. Then:

* while the entry's PR is open the feature waits on it, it is not stalled
  (open_entry_for, read by stall_check);
* once the entry merged, the feature is merged: its finished slices with
  it, every other slice closed (it was not built on its own), and a slice
  blocked on an owner decision stays blocked so the decision stays visible.
"""
import re

from .heartbeat import read_result

EVENT = "stall_resolver"
BRANCH_KEY = "carried_branch:"
PR_KEY = "carried_pr:"
DONE = ("merged", "closed")
LIVE_ENTRY = ("pr_open", "ci_fix", "blocked")


def _seen(orch):
    s = getattr(orch, "_carried_seen", None)
    if s is None:
        s = set()
        orch._carried_seen = s
    return s


def _own_branch(f, branch):
    fid = f["id"]
    return branch in (f.get("branch"), f"feat/{fid}") \
        or branch.startswith(f"sub/{fid}/")


def record(store, fid, data):
    """Remember the branch / PR a resolver of `fid` reported."""
    f = store.get_feature(fid)
    if f is None or not isinstance(data, dict):
        return False
    hit = False
    branch = data.get("branch")
    if isinstance(branch, str) and branch.strip() \
            and not _own_branch(f, branch.strip()):
        store.set_meta(BRANCH_KEY + branch.strip(), fid)
        hit = True
    pr = data.get("pr")
    if isinstance(pr, int) and not isinstance(pr, bool) and pr > 0:
        store.set_meta(PR_KEY + str(pr), fid)
        hit = True
    return hit


def backfill(orch, read=read_result):
    """Read the result of every finished resolver run not read yet."""
    store, seen = orch.store, _seen(orch)
    for w in store.tasks(kind="wake"):
        if w.get("event") != EVENT or not w.get("feature_id"):
            continue
        for r in store.runs_of(w["id"]):
            if r["id"] in seen or r.get("ended") is None:
                continue
            seen.add(r["id"])
            data, _ = read(r.get("result_path") or "")
            if data and record(store, w["feature_id"], data):
                store.event(w["id"], "carried_branch_recorded",
                            f"{w['feature_id']}: {data.get('branch')}")


def _names(title, fid):
    """True when the PR title starts with the feature id ('[partial] ' and
    the like may come first)."""
    return bool(re.match(r"^\s*(\[[^\]]*\]\s*)*" + re.escape(fid) + r"\s*:",
                         title or ""))


def entries(store):
    """Train entries adopted by number (Train.adopt: feature `pr-N`)."""
    return [f for f in store.features()
            if f.get("pr") and f["id"] == f"pr-{f['pr']}"]


def _open_feature(store, fid):
    f = store.get_feature(fid) if fid else None
    if f is None or f.get("pr") or f["state"] in DONE:
        return None
    return f


def open_features(store):
    """Features a carried PR may close: not finished, no PR of their own,
    and stalled once (a stall resolver was queued for them). Why the last:
    a contract PR titled with the feature id while its other slices build
    is not the feature's whole work."""
    stalled = {w.get("feature_id") for w in store.tasks(kind="wake")
               if w.get("event") == EVENT}
    return [f for f in store.features()
            if not f.get("pr") and f["state"] not in DONE
            and f["id"] in stalled]


def carried(store, entry, candidates=None):
    """The stuck feature entry's PR carries, or None. candidates: the
    open_features list, when the caller already has it."""
    fid = store.get_meta(BRANCH_KEY + (entry.get("branch") or "")) \
        or store.get_meta(PR_KEY + str(entry["pr"]))
    f = _open_feature(store, fid)
    if f is not None:
        return f
    title = (store.get_task(entry["id"]) or {}).get("brief") or ""
    if candidates is None:
        candidates = open_features(store)
    return next((c for c in candidates
                 if c["id"] != entry["id"] and _names(title, c["id"])), None)


def open_entry_for(store, fid):
    """The open train entry whose PR carries feature fid, or None."""
    for e in entries(store):
        if e["state"] not in LIVE_ENTRY:
            continue
        f = carried(store, e)
        if f is not None and f["id"] == fid:
            return e
    return None


def _running(store, fid):
    ids = {t["id"] for t in store.feature_tasks(fid)}
    return any(r["task_id"] in ids for r in store.active_runs())


def close_merged(orch, f, entry):
    """Feature f's work merged in entry's PR."""
    store, fid, n = orch.store, f["id"], entry["pr"]
    note = f"its work merged in PR #{n} ({entry['id']})"
    ts = store.feature_tasks(fid)
    with store.tx():
        for t in ts:
            if t["state"] in DONE or (t["state"] == "blocked"
                                      and t.get("owner")):
                continue
            if t["state"] == "integrating":
                store.advance(t["id"], "pr_open", note=note)
                store.advance(t["id"], "merged")
            else:
                store.advance(t["id"], "closed",
                              note=f"not built on its own; {note}")
        store.set_feature(fid, state="merged")
    for t in ts:
        orch._release(t)
    if ts:
        store.event(ts[0]["id"], "merged_elsewhere",
                    f"PR #{n} as {entry['id']}")


def sweep(orch):
    """Close the features a merged entry carried. Returns their ids."""
    store, done = orch.store, []
    backfill(orch)
    feats = open_features(store)
    for e in entries(store):
        if not feats:
            break
        if e["state"] != "merged":
            continue
        f = carried(store, e, feats)
        if f is None or not store.feature_tasks(f["id"]) \
                or _running(store, f["id"]):
            continue
        close_merged(orch, f, e)
        done.append(f["id"])
        feats = open_features(store)
    return done
