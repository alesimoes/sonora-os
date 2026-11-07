"""Actions that ask the running daemon for something: run a PR's GitHub checks again, rerun
one failed GitHub job, give a PR to a fixer or an integrator.

The dashboard never writes the daemon's state: each action drops a request
file (harness/dash_door.py) that the daemon applies on its next tick and
logs as an event. Refused unless a daemon is running, since nobody else
would read the request. The daemon checks the request again when it acts."""
from dashboard_actions import ActionError, register
from dashboard_actions_util import need_int, need_str, only, status_json
from dashboard_chat import daemon_alive, harness_dir
from dashboard_orch import failed_steps
from harness import dash_door


def _row(p):
    n = need_int(p, "pr")
    row = next((r for r in status_json().get("train") or [] if r.get("pr") == n), None)
    if row is None:
        raise ActionError("#%d não está no trem de merge" % n)
    if not daemon_alive():
        raise ActionError("o daemon não está rodando; o pedido não seria atendido")
    return n, row


def _v_regate(p):
    p = only(p, "pr")
    return {"pr": _row(p)[0]}


def _v_rerun(p):
    p = only(p, "pr", "step")
    n, row = _row(p)
    step = need_str(p, "step", 120)
    if step not in failed_steps(row):
        raise ActionError("%s não falhou no head atual de #%d" % (step, n))
    return {"pr": n, "step": step}


def _v_assign(p):
    p = only(p, "pr", "as")
    n, row = _row(p)
    if p.get("as") not in dash_door.ASSIGN_AS:
        raise ActionError("as deve ser fixer ou integrator")
    if row.get("fixer"):
        raise ActionError("#%d já tem o agente %s trabalhando" % (n, row["fixer"]))
    return {"pr": n, "as": p["as"]}


def _post(kind):
    def run(p, timeout):
        try:
            dash_door.post(harness_dir(), kind, **p)
        except (ValueError, OSError) as e:
            return False, str(e)
        return True, "pedido enviado ao daemon; ele age no próximo ciclo e registra o evento cli_request"
    return run


register("pr.regate", "prs", "rerodar checks no GitHub", _v_regate, _post("regate"), destructive=True, timeout=20)
register("pr.rerun_step", "prs", "rerodar job que falhou", _v_rerun, _post("rerun-step"), destructive=True, timeout=20)
register("pr.assign", "prs", "atribuir agente", _v_assign, _post("assign"), destructive=True, timeout=20)
