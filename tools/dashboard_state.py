"""The dashboard's shared snapshot and the loop that refreshes it. Each source
fails on its own: an error is listed in the snapshot, never raised."""
import json
import os
import threading
import time

from dashboard_agents import read_agents, stamp_done
from dashboard_disk import disk_view
from dashboard_github import read_merged, read_prs
from dashboard_machine import read_containers, read_lock, read_watchers
from dashboard_runview import add_last_steps
from dashboard_orch import clean_orch, read_coordinator, read_orchestrator, read_progress
from dashboard_usage import read_usage
from dashboard_why import with_why

GH_EVERY_S = 20
PROGRESS_EVERY_S = 300
LOOP_S = 4

state = {"usage": {"sessions": [], "week": None, "five_hour": None}, "updated": 0, "agents": [], "prs": [],
         "watchers": [], "containers": [], "errors": [], "orchestrator": None, "merged": [], "lock": [], "disk": None}
lock = threading.Lock()


def read_labels(agents_file, errors):
    try:
        if agents_file and os.path.exists(agents_file):
            with open(agents_file, encoding="utf-8") as f:
                return json.load(f)
    except Exception as e:
        errors.append("agents file: %s" % e)
    return {}


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


def refresh_once(agents_file, gh, progress):
    """One pass; `progress` is a one-slot dict {"at", "value"} owned by the loop."""
    errors = []
    gh.poll(errors)
    labels = read_labels(agents_file, errors)
    stamp_done(labels, agents_file)
    containers, watchers = read_containers(), read_watchers()
    lk = read_lock(labels)
    status = read_orchestrator()
    if time.time() - progress["at"] > PROGRESS_EVERY_S:
        try:
            progress["value"] = read_progress()
        except Exception as e:
            errors.append("progress: %s" % e)
        progress["at"] = time.time()
    orch = add_last_steps(clean_orch(status, {p["number"] for p in gh.prs} if gh.ok else None))
    prs = with_why(gh.prs, orch)
    with lock:
        state.update(updated=int(time.time()), progress=progress["value"], prs=prs, watchers=watchers,
                     containers=containers, merged=gh.merged, usage=read_usage(), orchestrator=orch,
                     coordinator=read_coordinator(), lock=lk, buildtime=None, disk=disk_view())
    agents = read_agents(labels, containers, lk)
    with lock:
        state.update(agents=agents, errors=errors)


def refresh_loop(agents_file):
    gh, progress = Gh(), {"at": 0.0, "value": None}
    while True:
        refresh_once(agents_file, gh, progress)
        time.sleep(LOOP_S)
