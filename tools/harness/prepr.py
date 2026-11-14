"""The daemon's run of tools/pre-pr.sh before it opens a feature PR.

Two things the bare run got wrong (ui.mixer.sidechain, 2026-11-12):
- pre-pr.sh refuses a kProtocolVersion change unless PRE_PR_ALLOW_PROTOCOL=1,
  which is meant for a coordinator-reserved bump; the daemon never set it, so
  a feature holding its reservation could never pass. The variable is set
  only from the store's reservation, so an unreserved bump still fails.
- the note kept the tail of the output; FAIL lines come first and size
  warnings pushed them out, so integrators saw only warnings.
"""

FAIL_MARK = "pre-pr: FAIL"


def allow_env(reserved):
    """Extra environment for pre-pr.sh from the feature's reservations
    ({"protocol": n|None, ...}, Flow.reserved_for). pre-pr.sh has no
    kFormatVersion check, so a format reservation needs no variable."""
    if reserved and reserved.get("protocol") is not None:
        return {"PRE_PR_ALLOW_PROTOCOL": "1"}
    return {}


def summary(out, limit):
    """At most `limit` chars of `out` with every FAIL line first, then as much
    of the tail of the rest as fits."""
    lines = (out or "").splitlines()
    fails = [ln for ln in lines if FAIL_MARK in ln]
    head = "\n".join(fails)
    if len(head) >= limit:
        return head[:limit]
    rest = "\n".join(ln for ln in lines if FAIL_MARK not in ln)
    room = limit - len(head) - (1 if head else 0)
    tail = rest[-room:] if room > 0 and rest else ""
    return "\n".join(x for x in (head, tail) if x)


def passes(o, fid, ts, wt, now):
    """Run pre-pr.sh in the feature worktree; on failure send every task of
    the feature back to integration with the FAIL lines in its note."""
    ok, out = o.forge.pre_pr(wt, env=allow_env(o.flow.reserved_for(fid)))
    if ok:
        return True
    rep = ts[0]
    o.store.event(rep["id"], "pre_pr_failed", summary(out, 500))
    note = "tools/pre-pr.sh failed; fix and commit:\n" + summary(out, 800)
    for x in ts:
        o.store.set_task(x["id"], integrated=0, note=note)
    o._count_failure(rep, "pre-pr failed", now)
    return False
