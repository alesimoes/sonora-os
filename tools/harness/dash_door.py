"""The dashboard's door to the running daemon (owner, 2026-11-07).

The daemon rewrites state.json from memory, so the dashboard never writes
it. It posts to the CLI mailbox (harness/cli_mailbox.py, .harness/cli-requests/)
and the daemon's next tick applies it on its own store (cli_mailbox.apply
calls dash_door.apply for these kinds):

  regate     queue the local gate again for the PR's current head
  rerun-step the same, with the named failed step run first, so a step that
             is still red stops the run early; every step still runs, a
             partial pass never counts
  assign     give the PR to a fixer (red checks) or an integrator (conflict)

Each applied request is a `cli_request` event, a refused one
`cli_request_failed`, in planning/orchestrator/events.jsonl.
"""
import re

KINDS = ("regate", "rerun-step", "assign")
ASSIGN_AS = {"fixer": "ci", "integrator": "conflict"}
STEP_RE = re.compile(r"^[a-z][a-z0-9-]{0,30}$")


class Refused(Exception):
    """The request cannot be applied now; the message says why."""


def post(hdir, kind, **args):
    """Queues one request on the daemon's CLI mailbox (harness/cli_mailbox.py)."""
    from . import cli_mailbox   # it imports this module for apply()
    return cli_mailbox.post(hdir, kind, **validate(kind, args))


def _train_row(orch, pr):
    info = orch.train.info(f"pr-{pr}")
    if not info:
        raise Refused(f"#{pr} is not on the merge train")
    return f"pr-{pr}", info


def _regate(orch, pr, step=None):
    _, info = _train_row(orch, pr)
    head = info.get("head")
    if not head:
        raise Refused(f"#{pr} has no head the train knows")
    r = orch.localci.result(head) or {}
    if r.get("state") == "running":
        raise Refused(f"the gate is already running for {head[:8]}")
    if step and f"FAIL {step}" not in (r.get("summary") or ""):
        raise Refused(f"{step} did not fail on {head[:8]}")
    orch.localci.forget(head, first=step)
    if not orch.localci.start(head):
        raise Refused("no gate slot; try again")
    return f"gate again for #{pr} {head[:8]}" + (f", {step} first" if step else "")


def _assign(orch, pr, who):
    fid, info = _train_row(orch, pr)
    f = orch.store.get_feature(fid)
    if not f or f.get("state") not in ("pr_open", "ci_fix"):
        raise Refused(f"#{pr} is not waiting for a fix (state {f and f.get('state')})")
    if any(r["name"] for r in orch.train.rows()
           if r["pr"] == pr and r.get("fixer")):
        raise Refused(f"#{pr} already has an agent working")
    orch.store.advance_feature(
        fid, "ci_fix", fix_kind=ASSIGN_AS[who], escalated=None,
        note=f"PR #{pr}: {who} assigned from the dashboard")
    return f"{who} assigned to #{pr}"


def apply(orch, kind, args):
    a = validate(kind, args)
    if kind == "regate":
        return _regate(orch, a["pr"])
    if kind == "rerun-step":
        return _regate(orch, a["pr"], a["step"])
    return _assign(orch, a["pr"], a["as"])
