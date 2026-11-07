"""The dashboard's door to the running daemon (owner, 2026-11-07).

The daemon rewrites state.json from memory, so the dashboard never writes
it. It posts to the CLI mailbox (harness/cli_mailbox.py, .harness/cli-requests/)
and the daemon's next tick applies it on its own store (cli_mailbox.apply
calls dash_door.apply for these kinds):

  regate     rerun the failed GitHub jobs of the PR's current head (the local
             gate was retired, so GitHub is the gate)
  assign     give the PR to a fixer (red checks) or an integrator (conflict)

Each applied request is a `cli_request` event, a refused one
`cli_request_failed`, in planning/orchestrator/events.jsonl.
"""

KINDS = ("regate", "assign")
ASSIGN_AS = {"fixer": "ci", "integrator": "conflict"}


class Refused(Exception):
    """The request cannot be applied now; the message says why."""


def validate(kind, args):
    """Cleaned args or ValueError; used by the poster and again by the daemon."""
    if kind not in KINDS:
        raise ValueError(f"unknown request {kind}")
    pr = args.get("pr")
    if isinstance(pr, bool) or not isinstance(pr, int) or pr <= 0:
        raise ValueError("pr must be a positive number")
    out = {"pr": pr}
    if kind == "assign":
        who = args.get("as")
        if who not in ASSIGN_AS:
            raise ValueError("as must be fixer or integrator")
        out["as"] = who
    return out


def post(hdir, kind, **args):
    """Queues one request on the daemon's CLI mailbox (harness/cli_mailbox.py)."""
    from . import cli_mailbox   # it imports this module for apply()
    return cli_mailbox.post(hdir, kind, **validate(kind, args))


def _train_row(orch, pr):
    info = orch.train.info(f"pr-{pr}")
    if not info:
        raise Refused(f"#{pr} is not on the merge train")
    return f"pr-{pr}", info


def _regate(orch, pr):
    _, info = _train_row(orch, pr)
    head = info.get("head")
    if not head:
        raise Refused(f"#{pr} has no head the train knows")
    if not orch.forge.rerun_failed(pr, head):
        raise Refused(f"#{pr} has no CI run to rerun for {head[:8]}")
    return f"CI rerun for #{pr} {head[:8]}"


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
    return _assign(orch, a["pr"], a["as"])
