"""The dashboard's door to the running daemon (owner, 2026-11-07).

The daemon rewrites state.json from memory, so the dashboard never writes
it. It posts to the CLI mailbox (harness/cli_mailbox.py, .harness/cli-requests/)
and the daemon's next tick applies it on its own store (cli_mailbox.apply
calls dash_door.apply for these kinds):

  regate     rerun the failed jobs of the PR head's GitHub run
             (forge.rerun_failed); the GitHub checks stay the whole gate
  rerun-step rerun only the named failed job (forge.rerun_job)
  assign     give the PR to a fixer (red checks) or an integrator (conflict)

Each applied request is a `cli_request` event, a refused one
`cli_request_failed`, in planning/orchestrator/events.jsonl.
"""

from .forge import ForgeError

KINDS = ("regate", "rerun-step", "assign")
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
    if kind == "rerun-step":
        step = args.get("step")
        if (not isinstance(step, str) or not 0 < len(step) <= 120
                or not step.isprintable()):
            raise ValueError("step must be a job name")
        out["step"] = step
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


def _regate(orch, pr, step=None):
    _, info = _train_row(orch, pr)
    head = info.get("head")
    if not head:
        raise Refused(f"#{pr} has no head the train knows")
    try:
        if step:
            if step not in (info.get("failing") or []):
                raise Refused(f"{step} did not fail on {head[:8]}")
            ok = orch.forge.rerun_job(pr, head, step)
        else:
            ok = orch.forge.rerun_failed(pr, head)
    except ForgeError as e:
        raise Refused(f"GitHub refused the rerun: {e}")
    if not ok:
        raise Refused(f"no finished run with a failed job for {head[:8]}")
    return f"rerun on GitHub for #{pr} {head[:8]}" + (f", {step} only" if step else "")


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
