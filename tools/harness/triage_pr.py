"""`gh pr create: No commits between main and <branch>` (owner 2026-11-08).

Opening the PR of a feature whose branch adds nothing to main means one of
two things, told apart by what the task is:

  verify / close / adr-pointer item  the work is already on main -> the
                                     feature is closed, nothing to merge;
  a build task                       the engineer never committed -> sent
                                     back once; the second time the
                                     principal engineer takes it over.
"""
import re

from . import triage

_NO_COMMITS = re.compile(r"no commits between\s+\S*main\b", re.I)
_ALREADY_KIND = re.compile(
    r"\b(verify|verif(y|ication)|close|closing|adr[-_ ]?pointer)\b", re.I)
MARK = "prnocommit"


def is_no_commits(err):
    return bool(_NO_COMMITS.search(str(err)))


def is_already_on_main_kind(ts):
    """The feature's tasks are verify/close/adr-pointer items, judged by
    id, branch and brief."""
    return any(_ALREADY_KIND.search(" ".join(
        str(t.get(k) or "") for k in ("id", "branch", "brief")))
        for t in ts)


def _close(o, f, ts, err):
    o.store.event(ts[0]["id"], "triage_pr_nocommit",
                  "closed: already on main")
    o._close_feature(f, "already on main (no commits between main and "
                        "the branch)")


def _send_back(o, f, ts, err, now):
    rep = ts[0]
    n = triage.count(rep.get("note"), MARK)
    if n >= 1:
        if o._can_escalate(rep):
            o._escalate(rep, "no-commits", now,
                        note="The branch still adds nothing to main. "
                             "Find why the work was not committed, finish "
                             "it and commit it.")
            for x in ts:
                o.store.set_task(x["id"], integrated=0)
            return True
        return False
    note = triage.mark(
        "The branch adds no commit over main: commit your work on the "
        "branch (git status, git add, git commit) before finishing.",
        MARK, n + 1)
    for x in ts:
        o.store.set_task(x["id"], integrated=0, note=note)
    o.store.event(rep["id"], "triage_pr_nocommit", "sent back once")
    return True


def handle(o, f, ts, err, now):
    """True when the error was dealt with (the caller does not count a PR
    failure); False leaves the normal retry/block path."""
    if not is_no_commits(err) or not ts:
        return False
    if is_already_on_main_kind(ts):
        _close(o, f, ts, err)
        return True
    return _send_back(o, f, ts, err, now)
