"""The only things the owner's console may do (owner, 2026-11-08).

A verb is a closed entry: `clean` (arguments or ValueError), `describe`
(card title, steps, effect, all written here, never taken from the model),
`target` (what two cards compete for), `run` (on the daemon's own store and
files). Nothing outside VERBS runs, whatever the console agent proposes.

  pool.pause / pool.resume   orchestrator.py pause --pool X [--resume]
  drain / stop / resume      orchestrator.py drain | stop | resume
  task.add                   orchestrator.py add
  pr.adopt                   orchestrator.py adopt
  agent.ask / agent.wake     harness/ownerchat.py
  run.stop                   end one agent run, hold its task 30 min
  task.requeue / task.close  put a task back in line / close it
"""
import argparse
import os
import re
from collections import namedtuple

from . import cli, closed, launcher, ownerchat
from . import protocol as P

Verb = namedtuple("Verb", "clean describe target run risk")
Card = namedtuple("Card", "title steps effect")

NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
STOP_HOLD_S = 1800.0
MAX_PRS = 10


def _str(a, key, rx=NAME_RE):
    v = a.get(key)
    if not isinstance(v, str) or not rx.match(v):
        raise ValueError(f"{key} invalid")
    return v


def _only(a, *keys):
    extra = set(a) - set(keys)
    if extra:
        raise ValueError("unknown argument: " + ", ".join(sorted(extra)))


def _none(a):
    _only(a)
    return {}


# -- switches (STOP files, paused.json): same code as the CLI ----------
def _switch(orch, cmd, **kw):
    cli._switches(argparse.Namespace(cmd=cmd, **kw), orch.odir)


def _pool(a):
    _only(a, "pool")
    return {"pool": _str(a, "pool")}


# -- tasks and runs -----------------------------------------------------
def _task_of(orch, a):
    t = orch.store.get_task(a["task"])
    if t is None:
        raise ValueError(f"no task {a['task']}")
    return t


def _live_run(orch, name):
    runs = [r for r in orch.store.active_runs() if r["name"] == name]
    if not runs:
        raise ValueError(f"{name} has no running run")
    return runs[-1]


def _stop_run(orch, a):
    r = _live_run(orch, a["name"])
    launcher.kill_tree(r["pid"])
    orch._end_run(r, "killed", None)
    orch.store.set_task(r["task_id"], not_before=orch.clock() + STOP_HOLD_S)
    orch.store.event(r["task_id"], "killed", f"{a['name']} stopped from the console")
    return f"{a['name']} stopped; task {r['task_id']} held 30 min"


def _requeue(orch, a):
    t = _task_of(orch, a)
    if t["state"] in ("merged", "closed"):
        raise ValueError(f"{t['id']} is {t['state']}, nothing to requeue")
    if t["state"] == "blocked":
        orch.store.advance(t["id"], "queued", note="requeued from the console")
    orch.store.set_task(t["id"], not_before=0, attempts=0)
    return f"{t['id']} back in line"


def _close(orch, a):
    t = _task_of(orch, a)
    f = orch.store.get_feature(t.get("feature_id") or "")
    if f and t["kind"] == "work":
        closed.close_feature(orch, f, "closed from the console")
    else:
        orch._release(t)
        orch.store.advance(t["id"], "closed", note="closed from the console")
    return f"{t['id']} closed"


def _add(orch, a):
    if orch.store.get_task(a["id"]) is not None:
        raise ValueError(f"task {a['id']} exists")
    if not os.path.isfile(os.path.join(orch.root, ".claude", "agents", a["agent"] + ".md")):
        raise ValueError(f"no agent {a['agent']}")
    orch.store.add_task(a["id"], a["track"], a["agent"], files=a["files"], brief=a["brief"])
    return f"task {a['id']} added"


def _clean_add(a):
    _only(a, "id", "track", "agent", "brief", "files")
    files = a.get("files") or []
    if not isinstance(files, list) or not all(isinstance(x, str) and x for x in files):
        raise ValueError("files must be a list of paths")
    return {"id": _str(a, "id"), "track": _str(a, "track"), "agent": _str(a, "agent"),
            "brief": str(a.get("brief") or "")[:2000], "files": files[:20]}


def _clean_prs(a):
    _only(a, "prs")
    prs = a.get("prs")
    if (not isinstance(prs, list) or not 0 < len(prs) <= MAX_PRS
            or not all(isinstance(n, int) and not isinstance(n, bool) and n > 0 for n in prs)):
        raise ValueError("prs must be 1 to 10 PR numbers")
    return {"prs": prs}


def _adopt(orch, a):
    got = orch.train.adopt(set(a["prs"]))
    return "adopted " + (" ".join(f"#{n}" for n in got) or "none")


# -- agents -------------------------------------------------------------
def _agent_run(orch, name):
    run = orch.store.last_run_of_name(name) if P.valid_name(name) else None
    if run is None:
        raise ValueError(f"{name} is not an agent of this orchestrator")
    return run


def _ask(orch, a):
    ownerchat.ask(orch, _agent_run(orch, a["name"]), a["text"])
    return f"message sent to {a['name']}"


def _wake(orch, a):
    got = ownerchat.wake(orch, _agent_run(orch, a["name"]))
    return f"{a['name']}: {got}"


def _clean_name(a):
    _only(a, "name")
    return {"name": _str(a, "name")}


def _clean_ask(a):
    _only(a, "name", "text")
    text = str(a.get("text") or "").strip()
    if not text:
        raise ValueError("text empty")
    return {"name": _str(a, "name"), "text": text[:ownerchat.MAX_TEXT]}


def _clean_task(a):
    _only(a, "task")
    return {"task": _str(a, "task")}


def _by(key):
    return lambda a: a[key]


def _pool_run(resume):
    def run(orch, a):
        _switch(orch, "pause", pool=a["pool"], resume=resume)
        return f"pool {a['pool']} " + ("resumed" if resume else "paused")
    return run


def _sw(cmd, done):
    def run(orch, a):
        _switch(orch, cmd)
        return done
    return run


VERBS = {
    "pool.pause": Verb(_pool, lambda a: Card(
        f"Pausar o pool {a['pool']}", [f"orchestrator.py pause --pool {a['pool']}"],
        "Nenhum lançamento novo nesse pool; quem já roda continua."),
        _by("pool"), _pool_run(False), False),
    "pool.resume": Verb(_pool, lambda a: Card(
        f"Retomar o pool {a['pool']}", [f"orchestrator.py pause --pool {a['pool']} --resume"],
        "Os lançamentos desse pool voltam."), _by("pool"), _pool_run(True), False),
    "drain": Verb(_none, lambda a: Card(
        "Esvaziar: parar de lançar", ["orchestrator.py drain"],
        "Nada novo é lançado; os agentes em execução terminam."),
        lambda a: "all", _sw("drain", "drain on"), True),
    "stop": Verb(_none, lambda a: Card(
        "Parar tudo agora", ["orchestrator.py stop"],
        "Todos os agentes em execução são encerrados e nada novo é lançado."),
        lambda a: "all", _sw("stop", "stop on"), True),
    "resume": Verb(_none, lambda a: Card(
        "Retomar lançamentos", ["orchestrator.py resume"],
        "Remove STOP e STOP.NOW; o daemon volta a lançar."),
        lambda a: "all", _sw("resume", "resumed"), False),
    "task.add": Verb(_clean_add, lambda a: Card(
        f"Criar a tarefa {a['id']}", [f"orchestrator.py add --id {a['id']} --track {a['track']} --agent {a['agent']}"],
        "Entra na fila; nada em execução muda."), _by("id"), _add, False),
    "pr.adopt": Verb(_clean_prs, lambda a: Card(
        "Adotar " + ", ".join(f"#{n}" for n in a["prs"]),
        ["orchestrator.py adopt " + " ".join(str(n) for n in a["prs"])],
        "Os PRs entram no trem de merge."), lambda a: "adopt", _adopt, False),
    "agent.ask": Verb(_clean_ask, lambda a: Card(
        f"Mandar mensagem ao {a['name']}", [f"owner_ask -> {a['name']}: {a['text'][:80]}"],
        "O agente lê entre duas chamadas de ferramenta e responde aqui."),
        _by("name"), _ask, False),
    "agent.wake": Verb(_clean_name, lambda a: Card(
        f"Relançar o {a['name']}", [f"owner wake {a['name']}"],
        "Se não roda, relança já, sem esperar o backoff; se roda, nada muda."),
        _by("name"), _wake, False),
    "run.stop": Verb(_clean_name, lambda a: Card(
        f"Parar {a['name']}", [f"encerrar a árvore de processos de {a['name']}",
                              "segurar a tarefa por 30 min"],
        "A branch e os arquivos ficam; só a execução acaba."),
        _by("name"), _stop_run, True),
    "task.requeue": Verb(_clean_task, lambda a: Card(
        f"Recolocar {a['task']} na fila", [f"tarefa {a['task']}: sem espera, tentativas a zero"],
        "Uma tarefa bloqueada volta a 'queued'."), _by("task"), _requeue, False),
    "task.close": Verb(_clean_task, lambda a: Card(
        f"Fechar {a['task']}", [f"tarefa {a['task']} (e a feature dela) -> closed"],
        "Estado final: não volta sozinha."), _by("task"), _close, True),
}
