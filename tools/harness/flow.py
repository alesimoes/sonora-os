"""Feature flow (ADR-0047 section 4a): planning -> distributing -> building ->
qa -> integrating -> pr_open -> merged, driven by event-driven architect and
lead runs.

The daemon owns the state. The architect only writes a plan file and the lead
only an assignment table; Flow validates both (harness/plans.py) and creates
the tasks itself, so an invalid file is rejected and the run relaunched
instead of obeyed. PR opening, CI and merge stay in the orchestrator (PR 2).
"""
import json
import os
import re

from . import dispatch, plangate, plans, queueimport, routing
from .forge import ForgeError
from .scheduler import HOLD
from .status import write_atomic

# Feature states in which the feature still needs planning or building work.
ACTIVE = ("planning", "distributing", "building", "qa", "integrating")
LOG_LINES = 80
LOG_CHARS = 9000


class Flow:
    def __init__(self, orch):
        self.o = orch

    # -- small helpers --------------------------------------------------
    @property
    def store(self):
        return self.o.store

    def _plan_dir(self):
        return os.path.join(self.o.hdir, "plan")

    def plan_file(self, fid):
        return os.path.join(self._plan_dir(), f"{fid}.plan.json")

    def assign_file(self, fid):
        return os.path.join(self._plan_dir(), f"{fid}.assignments.json")

    def accepted_plan_file(self, fid):
        return os.path.join(self.o.odir, "plans", f"{fid}.plan.json")

    def info(self, fid):
        try:
            return json.loads(self.store.get_meta(f"feature:{fid}", "{}"))
        except ValueError:
            return {}

    def accepted_plan(self, fid):
        try:
            with open(self.accepted_plan_file(fid), encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

    def is_planned(self, fid):
        f = self.store.get_feature(fid) if fid else None
        return bool(f and f["plan_path"])

    def owner(self, fid):
        return f"feature:{fid}"

    def reserved_for(self, fid):
        mine = [r for r in self.store.reservations()
                if r["task_id"] == self.owner(fid)]
        out = {"adr": None, "protocol": None, "formats": []}
        for r in mine:
            if r["kind"] in ("adr", "protocol"):
                out[r["kind"]] = r["number"]
            elif r["kind"] == "format":
                out["formats"].append(r["number"])
        return out

    # -- opening --------------------------------------------------------
    def open_feature(self, fid, track, backlog_ids, brief="", wants=("adr",),
                     waits_on="", state="queued"):
        """Create a feature in `queued`. Returns False when it exists."""
        if not re.match(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,47}$", fid):
            raise ValueError(f"bad feature id {fid!r}")
        if self.store.get_feature(fid) is not None:
            return False
        self.store.add_feature(fid, track, branch=f"feat/{fid}")
        self.store.set_feature(fid, state=state)
        self.store.set_meta(f"feature:{fid}", json.dumps({
            "backlog_ids": list(backlog_ids), "brief": brief,
            "wants": list(wants), "waits_on": waits_on, "track": track}))
        self.store.event(None, "feature_opened", f"{fid} {track}")
        return True

    def import_queue(self, text, wants=("adr",)):
        made = []
        for row in queueimport.parse(text):
            if self.open_feature(row["id"], row["track"], row["backlog_ids"],
                                 brief=row["brief"], wants=wants,
                                 waits_on=row["waits_on"]):
                made.append(row["id"])
        return made

    def on_proposal(self, body, sender):
        """The lead's pool_under_floor answer: a `plan_request` message to
        the daemon naming the next feature for a track."""
        if not isinstance(body, dict):
            return False
        fid, track = body.get("feature"), body.get("track")
        ids = body.get("backlog_ids")
        if not (isinstance(fid, str) and isinstance(ids, list) and ids
                and track in plans.TRACK_ENGINEERS):
            self.store.event(None, "proposal_rejected", f"{sender}: shape")
            return False
        if queueimport.out_of_scope(" ".join([fid, *map(str, ids)])):
            self.store.event(None, "proposal_rejected", f"{fid}: out of v1")
            return False
        try:
            made = self.open_feature(fid, track, [str(i) for i in ids],
                                     brief=str(body.get("brief") or ""),
                                     wants=body.get("wants") or ("adr",))
        except ValueError:
            made = False
        self.store.event(None, "proposal_accepted" if made
                         else "proposal_rejected", f"{sender}: {fid}")
        return made

    def _waits_unmet(self, fid):
        info = self.info(fid)
        text = info.get("waits_on") or ""
        for other in self.store.features():
            if other["id"] == fid or other["state"] in ("merged", "closed"):
                continue
            ids = {other["id"], *self.info(other["id"]).get("backlog_ids", [])}
            if any(i and i in text for i in ids):
                return True
        return False

    def promote_queued(self, now):
        """Open planning for queued features while there is room."""
        feats = self.store.features()
        active = sum(1 for f in feats if f["state"] in ACTIVE)
        for f in feats:
            if f["state"] != "queued":
                continue
            if active >= self.o.cfg.max_active_features:
                break
            if self._waits_unmet(f["id"]):
                continue
            if self.o._is_dry("plan"):
                key = ("promote", f["id"])
                if key not in self.o.dry_logged:
                    self.o.dry_logged.add(key)
                    self.store.event(None, "dry_run_promote", f["id"])
                continue
            if self.request_plan(f, now):
                active += 1

    def request_plan(self, f, now):
        fid, info = f["id"], self.info(f["id"])
        try:
            for kind in info.get("wants", ()):
                if kind in ("adr", "protocol", "format"):
                    self.store.reserve(kind, self.owner(fid),
                                       self.o.seed_fn(kind))
        except ForgeError as e:   # never guess a number
            self.store.event(None, "reserve_failed", f"{fid}: {e}")
            return False
        res = self.reserved_for(fid)
        self.store.set_feature(fid, state="planning", adr_no=res["adr"],
                               protocol_no=res["protocol"])
        self.o._ensure_wake(
            "architect", "plan_request", fid,
            brief=f"Plan feature {fid} ({f['track']}).", feature=fid,
            body={"feature": fid, "track": f["track"],
                  "backlog_ids": info.get("backlog_ids", []),
                  "reserved": res})
        return True

    # -- wake prompts ---------------------------------------------------
    def replay(self, fid):
        """The channel log of the feature: the only memory of a short run."""
        path = os.path.join(self.o.hdir, "log", "messages.jsonl")
        rows = []
        try:
            with open(path, encoding="utf-8") as f:
                for line in f:
                    try:
                        m = json.loads(line)
                    except ValueError:
                        continue    # a half-written last line
                    if m.get("feature") == fid:
                        rows.append(m)
        except OSError:
            return "(no channel log yet)"
        out = []
        for m in rows[-LOG_LINES:]:
            body = json.dumps(m.get("body"), sort_keys=True)
            out.append(f"[{m.get('ts', '?')}] {m.get('from')} -> "
                       f"{m.get('to')} {m.get('type')}"
                       f"{' task=' + m['task'] if m.get('task') else ''}: "
                       f"{body[:400]}")
        text = "\n".join(out) or "(channel log is empty)"
        return text[-LOG_CHARS:]

    def wake_context(self, t):
        fid, ev = t["feature_id"], t.get("event") or "wake"
        lines = [f"event: {ev} feature={fid or '-'}"]
        if not fid:
            return "\n".join(lines)
        f = self.store.get_feature(fid) or {}
        info = self.info(fid)
        res = self.reserved_for(fid)
        lines += [
            f"track: {f.get('track')}  feature branch: "
            f"{f.get('branch') or 'feat/' + fid}",
            f"backlog ids: {', '.join(info.get('backlog_ids', [])) or '-'}",
            f"reserved by the daemon (use exactly these, never pick one): "
            f"adr={res['adr']} protocol={res['protocol']} "
            f"formats={res['formats']}",
            f"plan schema: planning/orchestrator/plan.schema.json; "
            f"assignments schema: planning/orchestrator/"
            f"assignments.schema.json"]
        if ev == "plan_request":
            lines.append(f"Write the plan to {self.plan_file(fid)}. The "
                         "daemon validates it: no file-glob overlap between "
                         "tasks or with active features "
                         f"({', '.join(sorted(self.active_globs(fid))) or 'none'}"
                         "), contract first, depends_on acyclic. Group by "
                         "context (owner, 2026-11-04): backlog items of one "
                         "area that ship together go in this one feature "
                         "and its one PR, never one PR per item; say in "
                         "the plan which open items you grouped and why. "
                         + self.gate_hint(fid))
        elif ev == "plan_ready":
            lines.append(f"Read the plan at {self.plan_file(fid)}; write the "
                         f"table to {self.assign_file(fid)}. Sub-branch of "
                         f"task X is {plans.sub_branch(fid, 'X')}.")
        elif self.is_planned(fid):
            lines.append(f"Plan: {self.plan_file(fid)}; table: "
                         f"{self.assign_file(fid)} (rewrite it only when it "
                         "is now wrong).")
        lines += ["", "Channel log of this feature (replayed; everything "
                  "you remember):", self.replay(fid)]
        return "\n".join(lines)

    def gate_hint(self, fid):
        if not plangate.enabled(self.o.routing):
            return ""
        return (f"Also write planning/plans/{fid}.md from "
                "planning/plans/TEMPLATE.md and set `Status: approved` when "
                "the contracts and the ADR number are decided: the daemon "
                "starts no engineer without it. Give every task a `tier` "
                "(junior|pleno|senior) or a `grade` such as S/low.")

    # -- plan ingestion -------------------------------------------------
    def active_globs(self, exclude=None):
        """{feature: globs} of every other feature that still holds files."""
        out = {}
        for f in self.store.features():
            if f["id"] == exclude or f["state"] in ("merged", "blocked",
                                                    "queued", "closed"):
                continue
            globs = []
            raw = self.store.get_meta(f"planfiles:{f['id']}")
            if raw:
                globs = json.loads(raw)
            else:
                globs = [g for t in self.store.feature_tasks(f["id"])
                         for g in t["files"]]
            if globs:
                out[f["id"]] = globs
        return out

    def _read_json(self, path):
        try:
            with open(path, encoding="utf-8") as fh:
                return json.load(fh), None
        except FileNotFoundError:
            return None, f"{path} was not written"
        except ValueError as e:
            return None, f"{path} is not valid JSON: {e}"

    def ingest_plan(self, fid):
        """Returns (ok, errors). On ok the lead is woken."""
        plan, err = self._read_json(self.plan_file(fid))
        if err:
            return False, [err]
        f = self.store.get_feature(fid)
        res = self.reserved_for(fid)
        errs = plans.validate_plan(
            plan, {"feature": fid, "track": f["track"], "adr": res["adr"],
                   "protocol": res["protocol"], "formats": res["formats"]},
            self.active_globs(fid))
        if errs:
            self.store.event(None, "plan_invalid", f"{fid}: {errs[:6]}")
            return False, errs
        # No engineer for new feature work without the approved plan file
        # (adopted PRs never come through here).
        errs = plangate.check(self.o.root, fid, self.o.routing)
        if errs:
            self.store.event(None, "plan_not_approved", fid)
            return False, errs
        path = self.accepted_plan_file(fid)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        write_atomic(path, plan)
        self.store.set_meta(f"planfiles:{fid}", json.dumps(
            sorted({g for t in plan["tasks"] for g in t["files"]})))
        self.store.set_meta(f"merge_order:{fid}",
                            json.dumps(plan["merge_order"]))
        self.store.set_feature(
            fid, plan_path=path, branch=plan["feature_branch"],
            partial=int(plan["pr"]["partial"]), state="distributing",
            adr_no=plan["reserved"]["adr"],
            protocol_no=plan["reserved"]["protocol"])
        self.store.event(None, "plan_accepted",
                         f"{fid}: {len(plan['tasks'])} tasks")
        self.o._ensure_wake("lead", "plan_ready", fid, feature=fid,
                            brief=f"Distribute the plan of feature {fid}.",
                            body={"feature": fid, "plan": self.plan_file(fid)})
        return True, []

    # -- assignments ----------------------------------------------------
    def ingest_assignments(self, fid):
        """First valid table creates the tasks; a later one may reorder or
        reassign the tasks that have not started."""
        plan = self.accepted_plan(fid)
        if plan is None:
            return False, ["no accepted plan for this feature"]
        table, err = self._read_json(self.assign_file(fid))
        if err:
            return False, [err]
        raw = json.dumps(table, sort_keys=True)
        f = self.store.get_feature(fid)
        if f["state"] != "distributing" and \
                self.store.get_meta(f"assign_hash:{fid}") == raw:
            return True, []     # the lead left the table as it was
        errs = plans.validate_assignments(
            table, plan, routing.track_engineers(self.o.routing))
        if errs:
            self.store.event(None, "assignments_invalid",
                             f"{fid}: {errs[:6]}")
            return False, errs
        with self.store.tx():
            if f["state"] == "distributing":
                self._create_tasks(f, plan, table)
            else:
                self._reapply(fid, plan, table)
            self.store.set_meta(f"assign_hash:{fid}", raw)
        return True, []

    def _task_id(self, fid, tid):
        return f"{fid}.{tid}"

    def _brief(self, t, row):
        lines = [f"{t['title']} ({t['kind']})", "", t["brief"], "",
                 f"Start from: {row['start_from']}",
                 f"Your files (claimed): {', '.join(t['files'])}"]
        if t["accept"]:
            lines += ["Done when these pass:"] + [f"- {a}"
                                                  for a in t["accept"]]
        if t.get("needs_privileged"):
            lines.append("needs_privileged: list the privileged check as "
                         "unverified; the coordinator runs it.")
        return "\n".join(lines)

    def _create_tasks(self, f, plan, table):
        fid = f["id"]
        rows = {r["task"]: r for r in table["rows"]}
        held = self._dep_unmet(plan)
        for t in plan["tasks"]:
            row = rows[t["id"]]
            tid = self._task_id(fid, t["id"])
            self.store.add_task(
                tid, f["track"], row["engineer"], files=t["files"],
                brief=self._brief(t, row), feature_id=fid,
                backlog_id=",".join(plan["backlog_ids"]),
                branch=row["sub_branch"], priority=row["order"],
                depends_on=[self._task_id(fid, d) for d in t["depends_on"]])
            name = f"eng-{self.store.next_counter('name_engineer')}"
            self.store.set_task(
                tid, adr_no=plan["reserved"]["adr"],
                protocol_no=plan["reserved"]["protocol"], eng_name=name,
                start_from=row["start_from"],
                not_before=HOLD if held else 0)
            row["engineer_name"] = name
            dispatch.set_tier(self.o, tid, t.get("tier"), t.get("grade"))
        self.store.set_feature(fid, state="building")
        write_atomic(self.assign_file(fid), dict(table))
        self.store.event(None, "assignments_accepted",
                         f"{fid}: {len(rows)} rows")
        for t in plan["tasks"]:
            row = rows[t["id"]]
            self.o._tell(row["engineer_name"], "assign", {
                "task": self._task_id(fid, t["id"]),
                "sub_branch": row["sub_branch"], "order": row["order"],
                "start_from": row["start_from"], "accept": t["accept"]},
                feature=fid, task=self._task_id(fid, t["id"]))

    def _reapply(self, fid, plan, table):
        for r in table["rows"]:
            tid = self._task_id(fid, r["task"])
            cur = self.store.get_task(tid)
            if cur is None or cur["state"] != "queued":
                continue    # started work is never moved under an engineer
            self.store.set_task(tid, priority=r["order"], agent=r["engineer"])
        self.store.event(None, "assignments_updated", fid)

    def _dep_unmet(self, plan):
        dep = plan.get("depends_on_feature")
        if not dep:
            return False
        d = self.store.get_feature(dep)
        return d is None or d["state"] != "merged"

    # -- ticks ----------------------------------------------------------
    def tick(self, now):
        self.promote_queued(now)
        self._release_holds()
        self.sync_states()

    def _release_holds(self):
        for f in self.store.features():
            if f["state"] in ("merged", "blocked", "closed") or not f["plan_path"]:
                continue
            plan = self.accepted_plan(f["id"])
            if plan is None or self._dep_unmet(plan):
                continue
            for t in self.store.feature_tasks(f["id"]):
                if t["not_before"] >= HOLD:
                    self.store.set_task(t["id"], not_before=0)

    def sync_states(self):
        for f in self.store.features():
            if f["state"] not in ACTIVE:
                continue
            ts = self.store.feature_tasks(f["id"])
            if not ts:
                continue
            states = {t["state"] for t in ts}
            if states & {"queued", "building", "bugfix"}:
                new = "building"
            elif "qa" in states:
                new = "qa"
            elif states == {"integrating"}:
                new = "integrating"
            else:
                continue
            if new != f["state"]:
                self.store.set_feature(f["id"], state=new)

    # -- wake results ---------------------------------------------------
    def on_wake_done(self, t):
        """(ok, errors) for a finished architect/lead run. Events that carry
        no file (task_done, bug_ruling...) are always ok."""
        ev, fid = t.get("event"), t["feature_id"]
        if not fid:
            return True, []
        if ev == "plan_request":
            return self.ingest_plan(fid)
        if t["agent"] == "lead" and (ev == "plan_ready" or
                                     self.is_planned(fid)):
            if ev != "plan_ready" and not os.path.exists(self.assign_file(fid)):
                return True, []
            return self.ingest_assignments(fid)
        return True, []

    def on_wake_blocked(self, t):
        fid = t["feature_id"]
        if fid and t.get("event") in ("plan_request", "plan_ready"):
            self.store.set_feature(fid, state="blocked")
            self.store.event(None, "feature_blocked", f"{fid}: {t['note']}")
