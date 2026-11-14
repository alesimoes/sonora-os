"""Closure-only results do not open their own PR (owner, 2026-11-14: 22 of
35 overnight PRs carried no code).

A feature branch whose diff against origin/main touches only brain/,
planning/, docs/ or *.md is a closure: the item was already on main, only a
hardware/human run is left, or only an ADR close-out remains. The daemon
records it (item id, evidence commit, summary, the text of the files the
branch carries) in the `closures` meta list and closes the feature. One
batched PR, opened at most once every BATCH_EVERY_S, carries every pending
closure: a report under planning/closures/ plus the carried files that are
not on main yet. The BACKLOG ticks and progress stay with the coordinator's
chore PR, which reads that report.
"""
import json
import os

DOC_DIRS = ("brain/", "planning/", "docs/")
BATCH_EVERY_S = 4 * 3600
RETRY_S = 900
MAX_FILE = 200_000
BRANCH_PREFIX = "chore/closures-"
KEY, LAST = "closures", "closures:last"


def is_doc(path):
    p = path.replace("\\", "/")
    return p.startswith(DOC_DIRS) or p.endswith(".md")


def closure_only(files):
    """True for a diff that ships no code; None (unknown) is never one."""
    return files is not None and all(is_doc(p) for p in files)


def pending(store):
    try:
        return json.loads(store.get_meta(KEY, "[]"))
    except ValueError:
        return []


def _carried(wt, files):
    out = {}
    for p in files:
        full = os.path.join(wt, p)
        try:
            if os.path.getsize(full) <= MAX_FILE:
                with open(full, encoding="utf-8") as f:
                    out[p] = f.read()
        except (OSError, ValueError):
            pass
    return out


def record(store, f, ts, wt, files, now):
    try:
        ready = json.loads(store.get_meta(f"ready:{f['id']}", "{}"))
    except ValueError:
        ready = {}
    entry = {"id": f["id"], "at": now, "evidence": ready.get("commit"),
             "summary": (ready.get("summary") or "").strip(),
             "followups": ready.get("followups") or [],
             "unverified": ready.get("unverified") or [],
             "files": _carried(wt, files)}
    store.set_meta(KEY, json.dumps(pending(store) + [entry]))
    return entry


def route(o, f, ts, wt, branch, now):
    """True when feature `f` was a closure: recorded and closed, no PR."""
    files = o.forge.changed_files(None, branch)
    if not closure_only(files):
        return False
    record(o.store, f, ts, wt, files, now)
    note = "closure recorded; no PR (batched in the closures PR)"
    o.store.advance_feature(f["id"], "closed", note=note)
    for x in ts:
        o._release(x)
    o.store.event(ts[0]["id"], "closure_recorded", f"{f['id']} {branch}")
    return True


def report(entries):
    lines = ["# Closures", "",
             "Items closed without code; the coordinator ticks BACKLOG and "
             "progress from this list.", ""]
    for e in entries:
        lines.append(f"## {e['id']}")
        lines.append(f"- Evidence: {e['evidence'] or 'none recorded'}")
        lines.append(f"- {e['summary'] or 'no summary'}")
        lines += [f"- Follow-up: {x}" for x in e["followups"]]
        lines += [f"- Unverified: {x}" for x in e["unverified"]]
        lines += [f"- Carries: {p}" for p in sorted(e["files"])]
        lines.append("")
    return "\n".join(lines)


def _write(wt, entries, stamp):
    rel = f"planning/closures/{stamp}.md"
    paths = [rel]
    for e in entries:
        for p, text in e["files"].items():
            full = os.path.join(wt, p)
            if not os.path.exists(full):   # never overwrite what main has
                os.makedirs(os.path.dirname(full), exist_ok=True)
                with open(full, "w", encoding="utf-8", newline="\n") as fh:
                    fh.write(text)
                paths.append(p)
    full = os.path.join(wt, rel)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(report(entries))
    return paths


def due(o, now):
    if not pending(o.store):
        return False
    if now < float(o.store.get_meta(LAST, 0)) + BATCH_EVERY_S:
        return False
    return not any(p["branch"].startswith(BRANCH_PREFIX)
                   for p in o.forge.open_prs())


def open_batch(o, now):
    """One PR with every pending closure; None when nothing is due."""
    if not due(o, now):
        return None
    entries = pending(o.store)
    stamp = time_stamp(now)
    branch = BRANCH_PREFIX + stamp
    try:
        wt = o.worktrees.ensure_pr(f"closures-{stamp}", branch,
                                   start="origin/main")
        _write(wt, entries, stamp)
        o.forge.commit_all(wt, f"closures: {len(entries)} item(s) closed "
                               "without code")
        o.forge.push_head(wt, branch)
        ids = ", ".join(e["id"] for e in entries)
        body = ["## Closures", "", "Items closed without code, batched "
                "(owner, 2026-11-14):"]
        body += [f"- {e['id']}: {e['summary'].splitlines()[0] if e['summary'] else ''}"
                 for e in entries]
        body += ["", getattr(o, "attribution", "")]
        n = o.forge.create_pr(branch, "main", f"closures: {ids}"[:100],
                              "\n".join(body))
        if not o.cfg.daemon_merge:
            o.forge.enqueue(n)
    except Exception as e:   # keep the list; retry later
        o.store.set_meta(LAST, now - BATCH_EVERY_S + RETRY_S)
        o.store.event(None, "closures_pr_failed", str(e)[:300])
        return None
    o.store.set_meta(KEY, "[]")
    o.store.set_meta(LAST, now)
    return n


def time_stamp(now):
    import time
    return time.strftime("%Y%m%d-%H%M%S", time.gmtime(now))
