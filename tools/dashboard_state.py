"""The console's snapshot and the loop that refreshes it: the daemon's
status.json, the open and merged PRs from GitHub, and the board rows built
from them. Each source fails on its own: an error is listed, never raised."""
import threading
import time

from dashboard_board import agent_rows
from dashboard_github import read_merged, read_prs
from dashboard_orch import clean_orch, read_coordinator_agents, read_orchestrator
from dashboard_prboard import GreenClock, pr_rows

GH_EVERY_S = 20
LOOP_S = 4

state = {"updated": 0, "prs": [], "merged": [], "errors": [], "orchestrator": None,
         "board": {"agents": [], "prs": []}}
lock = threading.Lock()
green_clock = GreenClock()


def build_board(orch, prs, now=None):
    """Every running agent and the open PRs with their gate."""
    now = now or time.time()
    return {"agents": agent_rows(orch, read_coordinator_agents(now), [], now),
            "prs": pr_rows(prs, orch, green_clock, now)}


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


def refresh_once(gh):
    errors = []
    gh.poll(errors)
    orch = clean_orch(read_orchestrator(), {p["number"] for p in gh.prs} if gh.ok else None)
    try:
        board = build_board(orch, gh.prs)
    except Exception as e:
        board = state["board"]
        errors.append("quadro: %s" % e)
    with lock:
        state.update(updated=int(time.time()), prs=gh.prs, merged=gh.merged,
                     orchestrator=orch, board=board, errors=errors)


def refresh_loop():
    gh = Gh()
    while True:
        refresh_once(gh)
        time.sleep(LOOP_S)
