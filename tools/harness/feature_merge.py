"""One merge owner for a PR the daemon opened (2026-11-08: #507, #509 and
#510 sat green and CLEAN for an hour).

The feature path used to rely on a one-shot detached `enqueue-pr.sh`; when
that script skipped a PR nothing else would merge it, and the train only
merges adopted PRs. Now the watch loop merges a feature PR itself, on the
same rule the train uses: every check passed, the CI `Gate` among them,
GitHub says it is mergeable, the base is main. The merge is pinned to the
head that was judged, so a push in between makes GitHub refuse it.
"""
from .forge import ForgeError

GATE = "Gate"


def ready_to_merge(detail):
    """(ok, why). Pure, so the rule is testable without a forge."""
    if detail["state"] != "open":
        return False, "not open"
    if detail.get("base") not in (None, "", "main"):
        return False, "base is not main"
    checks = detail["checks"]
    if not any(c["name"] == GATE for c in checks):
        return False, "Gate not reported"
    if any(c["state"] != "pass" for c in checks):
        return False, "checks not all green"
    if detail["mergeable"] != "yes":
        return False, "not mergeable (yet)"
    return True, ""


def merge_if_green(orch, f, ts):
    """Merge feature `f`'s PR when it is green and clean. True when merged."""
    n = f["pr"]
    if orch._is_dry("merge"):
        return False
    try:
        d = orch.forge.pr_detail(n)
        ok, _ = ready_to_merge(d)
        if not ok:
            return False
        orch.forge.merge(n, d["head_sha"])
    except ForgeError as e:
        orch.store.event(ts[0]["id"], "feature_merge_failed", str(e)[:200])
        return False
    orch.store.event(ts[0]["id"], "feature_pr_merged",
                     f"#{n} {d['head_sha'][:8]}")
    orch._merged(f, ts)
    return True
