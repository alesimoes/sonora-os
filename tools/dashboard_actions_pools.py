"""Resume several paused pools with one click: the whole construction
(build, build:daw, build:os, build:plugin) or every paused pool. Each pool
goes through the orchestrator's own `pause --pool X --resume`."""
from dashboard_actions import ActionError, register
from dashboard_actions_util import need_str, only, orch_cli, status_json
from dashboard_runview import live_paused

BUILD_POOLS = ("build", "build:daw", "build:os", "build:plugin")


def pools_of(group, paused):
    """The paused pools a group names, in a stable order."""
    if group == "all":
        return sorted(paused)
    return [n for n in BUILD_POOLS if n in paused]


def _v_group(p):
    p = only(p, "group")
    if need_str(p, "group", 10) not in ("build", "all"):
        raise ActionError("group must be build or all")
    return p


def _run(p, timeout):
    paused = live_paused()
    if paused is None:
        paused = status_json().get("paused") or []
    todo = pools_of(p["group"], paused)
    if not todo:
        return True, "nenhuma fila pausada nesse grupo"
    ok, lines = True, []
    for n in todo:
        rc, out = orch_cli(["pause", "--pool", n, "--resume"], timeout)
        ok = ok and rc == 0
        lines.append("%s: %s" % (n, out.strip() or ("rc=%s" % rc)))
    return ok, "\n".join(lines)


register("pool.resume_group", "pools", "retomar várias filas", _v_group,
         _run, timeout=60)
