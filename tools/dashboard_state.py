"""The console's snapshot and the loop that refreshes it: the daemon's
status.json, the open and merged PRs from GitHub, and the board rows built
from them. Each source fails on its own: an error is listed, never raised."""
import os
import threading
import time

from dashboard_board import agent_rows
from dashboard_cost import read_cost
from dashboard_github import read_merged, read_prs
from dashboard_orch import clean_orch, read_coordinator_agents, read_orchestrator
from dashboard_prboard import GreenClock, pr_rows
from dashboard_progress import EVERY_S as PROGRESS_EVERY_S, read_progress
from dashboard_util import orch_root

GH_EVERY_S = 20
LOOP_S = 4
COST_EVERY_S = 30
_cost_at = [0.0]

state = {"updated": 0, "progress": None, "prs": [], "merged": [], "errors": [], "orchestrator": None,
         "board": {"agents": [], "prs": []}}
lock = threading.Lock()
green_clock = GreenClock()


def build_board(orch, prs, now=None):
    """Every running agent and the open PRs with their gate."""
    now = now or time.time()
    return {"agents": agent_rows(orch, read_coordinator_agents(now), [], now),
            "prs": pr_rows(prs, orch, green_clock, now)}


class Progress:
    """The catalogue, re-read every PROGRESS_EVERY_S; the last good value stands."""
    def __init__(self):
        self.at, self.value = 0.0, None

    def poll(self, errors):
        if time.time() - self.at <= PROGRESS_EVERY_S:
            return
        self.at = time.time()
        try:
            self.value = read_progress() or self.value
        except Exception as e:
            errors.append("progresso: %s" % e)


class Gh:
    """GitHub, read at most every GH_EVERY_S; the last good answer stands in between."""
    def __init__(self):
        self.at, self.prs, self.merged, self.ok = 0.0, [], [], False

    def poll(self, errors):
        try:
            if time.time() - self.at > GH_EVERY_S:
                self.prs, self.merged = read_prs(), read_merged()
                self.at, self.ok = time.time(), True
        except Exception as e:
            errors.append("gh: %s" % e)


def refresh_once(gh, prog=None):
    errors = []
    gh.poll(errors)
    if prog:
        prog.poll(errors)
    orch = clean_orch(read_orchestrator(), {p["number"] for p in gh.prs} if gh.ok else None)
    try:
        board = build_board(orch, gh.prs)
    except Exception as e:
        board = state["board"]
        errors.append("quadro: %s" % e)
    cost = state.get("cost")
    if time.time() - _cost_at[0] >= COST_EVERY_S:
        _cost_at[0] = time.time()
        try:
            cost = read_cost(os.path.join(orch_root(), ".harness", "runs")) or cost
        except Exception as e:
            errors.append("custo: %s" % e)
    with lock:
        state.update(updated=int(time.time()), prs=gh.prs, merged=gh.merged,
                     orchestrator=orch, board=board, errors=errors, cost=cost,
                     progress=prog.value if prog else state["progress"])


def refresh_loop():
    gh, prog = Gh(), Progress()
    while True:
        refresh_once(gh, prog)
        time.sleep(LOOP_S)
