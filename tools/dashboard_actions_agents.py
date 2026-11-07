"""Actions on agent runs: stop one (its process tree) or relaunch its task.

Only runs the orchestrator's state.json lists, still alive, can be stopped;
the daemon sees the dead run and relaunches it by its own backoff rules.
Relaunch is the owner "wake" request the daemon serves on its next tick."""
from dashboard_actions import ActionError, register
from dashboard_actions_util import need_str, only
from dashboard_chat import daemon_alive, harness_dir
from dashboard_orch import orch_state
from harness import launcher, ownerchat, proctree


def live_run(name):
    runs = [r for r in orch_state().get("runs", []) if r.get("name") == name and not r.get("ended")]
    if not runs:
        raise ActionError("não há execução ativa de %s no orquestrador" % name)
    return runs[-1]


def _v_stop(p):
    p = only(p, "name")
    need_str(p, "name")
    run = live_run(p["name"])
    if not launcher.pid_alive(run.get("pid")):
        raise ActionError("o processo de %s já terminou" % p["name"])
    return p


def _stop(p, timeout):
    run = live_run(p["name"])
    pid = run.get("pid")
    if not launcher.pid_alive(pid):
        return False, "o processo já terminou"
    proctree.kill_tree(pid)
    return True, "árvore do pid %s encerrada; o daemon relança pelas regras de backoff" % pid


def _v_wake(p):
    p = only(p, "name")
    need_str(p, "name")
    runs = [r for r in orch_state().get("runs", []) if r.get("name") == p["name"]]
    if not runs:
        raise ActionError("agente desconhecido do orquestrador")
    if not runs[-1].get("ended") and launcher.pid_alive(runs[-1].get("pid")):
        raise ActionError("%s já está rodando" % p["name"])
    if not daemon_alive():
        raise ActionError("o daemon não está rodando; o pedido não seria atendido")
    return p


def _wake(p, timeout):
    ownerchat.write_request(harness_dir(), "wake", p["name"], "relançar pelo painel")
    return True, "pedido de relançamento enfileirado para o daemon"


register("run.stop", "agents", "parar execução", _v_stop, _stop, destructive=True, timeout=45)
register("run.relaunch", "agents", "relançar tarefa", _v_wake, _wake)
