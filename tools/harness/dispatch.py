"""Which agent and which model a run gets (owner process, 2026-11-06).

Glue between the orchestrator and routing/tiers/failures: every function takes
the orchestrator and reads only its store, root and routing. With no
routing.toml each one answers what the daemon did before: the task's own
agent, sonnet, and no senior round.
"""
import json
import os

from . import failures, tiers

PRINCIPAL_AGENT = "principal-engineer"


def _agent_exists(orch, agent):
    return os.path.isfile(os.path.join(orch.root, ".claude", "agents",
                                       agent + ".md"))


def feature_files(orch, t):
    fid = t.get("feature_id")
    if not fid:
        return list(t.get("files") or ())
    return sorted({f for x in orch.store.feature_tasks(fid)
                   for f in x["files"]})


def agent_for(orch, t, kind):
    """The agent for one run. A routed agent whose file the daemon checkout
    does not have yet is ignored (`--agent` would die with "not found")."""
    if kind == "principal":
        return PRINCIPAL_AGENT
    fixed = {"tester": "tester", "integrator": "integrator"}.get(kind)
    if fixed:
        return fixed
    routing = orch.routing
    if routing is None or t["kind"] != "work":
        return t["agent"]
    files = feature_files(orch, t)
    if kind == "fixer":
        fk = failures.classify(t.get("note") or "")
        agent = failures.agent_for(routing, fk, files)
    else:
        agent = t["agent"] or routing.agent_for_files(files)
    return agent if agent and _agent_exists(orch, agent) else t["agent"]


# -- tiers ---------------------------------------------------------------
def set_tier(orch, task_id, tier=None, grade=None):
    if tier or grade:
        orch.store.set_meta(f"tier:{task_id}", json.dumps(
            {"tier": tier, "grade": grade}))


def tier_of(orch, task_id):
    try:
        d = json.loads(orch.store.get_meta(f"tier:{task_id}", "{}"))
    except ValueError:
        d = {}
    return d.get("tier"), d.get("grade")


def model_for(orch, t, kind):
    """`--model` for one run; None leaves it to the launcher (principal)."""
    if kind == "principal":
        return None
    tier, grade = tier_of(orch, t["id"])
    senior = kind == "fixer" and senior_round_active(orch, t)
    return tiers.run_model(orch.routing, kind, tier, grade, senior)


# -- escalation: senior tier before the principal ------------------------
def _senior_key(fid):
    return f"senior_round:{fid}"


def senior_round_active(orch, t):
    fid = t.get("feature_id")
    return bool(fid and orch.store.get_meta(_senior_key(fid)))


def take_senior_round(orch, fid):
    """True once per red spell when the same red has used its fix rounds: the
    next fixer runs on the senior tier of the same area agent. False without
    routing.toml (then the principal comes first, as before) and after the
    senior round was spent."""
    if orch.routing is None or orch.store.get_meta(_senior_key(fid)):
        return False
    orch.store.set_meta(_senior_key(fid), "1")
    orch.store.event(None, "senior_round", fid)
    return True


def clear_senior_round(orch, fid):
    if orch.store.get_meta(_senior_key(fid)):
        orch.store.set_meta(_senior_key(fid), "")
