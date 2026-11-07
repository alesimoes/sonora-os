"""Actions on pull requests: adopt one onto the merge train (the orchestrator
CLI, whichever mechanism main has) and merge one now.

Merge only when everything agrees at the moment of the click: the train row
says the local gate is green (phase `verde`) for the head it knows, GitHub
says the PR is open, mergeable, against main, and still at that head. The
merge itself is pinned to that head with --match-head-commit."""
import json

import dashboard_proc
from dashboard_actions import ActionError, register
from dashboard_actions_util import need_int, only, orch_cli, result, status_json
from dashboard_state import state


def known_prs():
    return {p["number"]: p for p in state.get("prs") or []}


def _open_pr(p):
    p = only(p, "pr")
    n = need_int(p, "pr")
    if n not in known_prs():
        raise ActionError("#%d não é um PR aberto" % n)
    return {"pr": n}


def _adopt(p, timeout):
    return result(*orch_cli(["adopt", str(p["pr"])], timeout))


def train_row(n):
    return next((r for r in status_json().get("train") or [] if r.get("pr") == n), None)


def merge_precheck(n, gh_view):
    """The head to merge, or ActionError saying which condition fails."""
    row = train_row(n)
    if not row or row.get("phase") != "verde":
        raise ActionError("a porta local não está verde para #%d (fase: %s)" % (n, (row or {}).get("phase")))
    if gh_view.get("state") != "OPEN":
        raise ActionError("#%d não está aberto" % n)
    if gh_view.get("baseRefName") != "main":
        raise ActionError("#%d não aponta para main" % n)
    if gh_view.get("mergeable") != "MERGEABLE":
        raise ActionError("#%d não é mesclável (%s)" % (n, gh_view.get("mergeable")))
    head = gh_view.get("headRefOid")
    if not head or (row.get("head") and row["head"] != head):
        raise ActionError("o head de #%d mudou desde que a porta ficou verde" % n)
    return head


def gh_pr_view(n, timeout):
    rc, out = dashboard_proc.run_cmd(["gh", "pr", "view", str(n), "--json", "state,mergeable,baseRefName,headRefOid"], timeout)
    if rc != 0:
        raise ActionError("gh pr view falhou: " + out.strip()[-200:])
    try:
        return json.loads(out)
    except ValueError:
        raise ActionError("gh devolveu algo ilegível")


def _v_merge(p):
    p = _open_pr(p)
    merge_precheck(p["pr"], gh_pr_view(p["pr"], 30))
    return p


def _merge(p, timeout):
    n = p["pr"]
    try:
        head = merge_precheck(n, gh_pr_view(n, timeout))   # again: the head may have moved since the click
    except ActionError as e:
        return False, str(e)
    return result(*dashboard_proc.run_cmd(["gh", "pr", "merge", str(n), "--squash", "--match-head-commit", head], timeout))


register("pr.adopt", "prs", "adotar no trem", _open_pr, _adopt, timeout=60)
register("pr.merge", "prs", "merge agora", _v_merge, _merge, destructive=True, timeout=90)
