"""Triage of an agent's `blocked` result (owner 2026-11-08).

18 tasks sat blocked for hours for harness reasons, not code. Each class
below is recognised from the result text and acted on by the daemon, so no
human has to look:

  infra     broker down / WinError 10061, a command limit hit waiting on
            tools/build-slot.sh, docker engine errors -> requeue with
            backoff, attempts kept
  nocommit  "No commits between main and <branch>" -> close as already on
            main when the task's files are there, else requeue once
  scope     the work is green (accept, tests, QA or "functionally green"),
            blocked only on files outside its claim -> the claim is widened
            with every path named and the task goes to QA. A path another
            live feature holds is widened too: the edit is already made,
            and claims serialise the next engineer/integrator launch
            (claim_for at launch time), so holding the task gained nothing
            (2026-11-08: interchange.stems sat blocked 3 h because one of
            six paths, a one-line test_control.cpp hunk, was held). A report
            that names no path still wakes the architect and requeues.
  decided   the agent reported blocked on a question that a `decision` or
            `answer` in its mailbox already settles (the lead answered in
            the run's last minute and the run ended without acting on it)
            -> requeued once with the decision in the note
  (the janitor 'container running' retry lives in blockinfo.)

`classify` is pure; `handle` applies the action and returns True when it
dealt with the result (the caller then does not block)."""
import json
import re

RETRY_S = 300.0        # first infra backoff; doubles per retry
RETRY_MAX_S = 3600.0
PLAN_WAIT_S = 900.0    # a scope requeue waits for the architect this long
LIVE_OTHER = ("queued", "building", "bugfix", "qa", "integrating",
              "pr_open", "ci_fix", "designing")
DECISION_TYPES = ("decision", "answer")

_INFRA = re.compile(
    r"WinError 10061|broker (is )?(unreachable|down)|connection refused"
    r"|10[- ]minute (command )?(limit|timeout)|command (time ?limit|timed out)"
    r".{0,80}build-slot|build-slot.{0,120}(time ?limit|timed? ?out|10 min)"
    r"|(docker|container) (engine|daemon)[^.\n]{0,40}(error|not running|"
    r"unreachable|unavailable)|cannot connect to the docker daemon"
    r"|error during connect.{0,60}docker", re.I | re.S)
_NOCOMMIT = re.compile(r"no commits between\s+\S*main\b", re.I)
_ACCEPT_OK = re.compile(
    r"accept(ance)?[^.\n]{0,40}\b(pass(es|ed|ing)?|green|ok)\b", re.I)
# The same news in an engineer's own words: "functionally green per
# tester-113", "tests pass", "QA passed", "code is green", "ctest ... Passed".
_GREEN = re.compile(
    r"\b(functionally|code(-wise)?|tests?|tester-?\d*|qa|ctest|build)\b"
    r"[^.\n]{0,60}\b(pass(es|ed|ing)?|green|ok)\b", re.I)
# The tester failed on scope and nothing else.
_SCOPE_ONLY = re.compile(
    r"\b(fail(ed|s)?|blocked)\s+(only\s+)?on\s+(a\s+|the\s+)?scope\b"
    r"|\bscope(\s+(decision|finding|issue))?\s+only\b", re.I)
_SCOPE = re.compile(r"\b(scope|outside (its|my|the|our) claim|"
                    r"not in (its|my|the) claim|outside (its|my|the) files|"
                    r"files? outside|unclaimed files?)", re.I)
_PATH = re.compile(r"[\w.+-]+(?:/[\w.+-]+)+")
_MARK = re.compile(r"\[triage:(\w+) (\d+)\]")


def classify(role, summary):
    s = summary or ""
    if _INFRA.search(s):
        return "infra"
    if _NOCOMMIT.search(s):
        return "nocommit"
    if role == "engineer" and _SCOPE.search(s) and (
            _ACCEPT_OK.search(s) or _GREEN.search(s) or _SCOPE_ONLY.search(s)):
        return "scope"
    return None


def count(note, cls):
    """How many times this class already acted, from the task's note."""
    for c, n in _MARK.findall(note or ""):
        if c == cls:
            return int(n)
    return 0


def mark(note, cls, n):
    base = _MARK.sub("", note or "").strip()
    others = " ".join(f"[triage:{c} {k}]" for c, k in _MARK.findall(note or "")
                      if c != cls)
    return " ".join(x for x in (base, others, f"[triage:{cls} {n}]") if x)


def paths_in(summary):
    seen = []
    for p in _PATH.findall(summary or ""):
        p = p.rstrip(".,;:")   # "..., test_control.cpp." ends a sentence
        if "." in p.rsplit("/", 1)[-1] and p not in seen:
            seen.append(p)
    return seen


def _requeue(o, t, now, why, cls, delay, note=None):
    n = count(t.get("note"), cls) + 1
    o.store.set_task(t["id"], not_before=now + delay,
                     note=mark(note if note is not None else t.get("note"),
                               cls, n))
    o.store.event(t["id"], "triage_" + cls, why[:200])


def _infra(o, t, why, now):
    n = count(t.get("note"), "infra")
    _requeue(o, t, now, why, "infra", min(RETRY_S * 2 ** n, RETRY_MAX_S))


def on_main(o, t, data):
    """The task's work is already on origin/main: every file is there and
    a commit the engineer reported (if any) is in main's history."""
    files = t.get("files") or []
    if not files:
        return False
    try:
        if not all(o.forge.main_files(f) for f in files):
            return False
        sha = (data or {}).get("commit")
        return not sha or bool(o.forge.is_ancestor(sha, "origin/main"))
    except Exception:  # noqa: BLE001 - unknown is not "on main"
        return False


def _nocommit(o, t, why, data, now):
    if on_main(o, t, data):
        o._release(t)
        o.store.advance(t["id"], "closed", note="already on main")
        o.store.event(t["id"], "triage_nocommit", "closed: already on main")
        return True
    if count(t.get("note"), "nocommit") >= 1:
        return False
    _requeue(o, t, now, why, "nocommit", 0,
             note="No commits reached main from the previous run; commit "
                  "your work on the branch and push it.")
    return True


def held_elsewhere(o, t, paths):
    """Live tasks of other features whose files meet `paths`: 'path (task)'."""
    out = []
    same = t.get("feature_id")
    for x in o.store.tasks():
        if x["id"] == t["id"] or x["kind"] != "work" \
                or x["state"] not in LIVE_OTHER:
            continue
        if same is not None and x.get("feature_id") == same:
            continue
        for p in sorted(set(x.get("files") or []) & set(paths)):
            out.append(f"{p} ({x['id']})")
    return out


def _scope(o, t, why, now):
    paths = [p for p in paths_in(why) if p not in (t.get("files") or [])]
    if paths:
        held = held_elsewhere(o, t, paths)
        o.store.set_task(t["id"], files=list(t["files"]) + paths)
        o.store.advance(t["id"], "qa",
                        note="claim widened: " + ", ".join(paths))
        o._release(t)
        o.store.event(t["id"], "triage_scope", "widened " + ", ".join(paths)
                      + ("; also held by: " + ", ".join(held) if held else ""))
        o._tell("lead", "task_done", {"task": t["id"]},
                feature=t["feature_id"], task=t["id"])
        return True
    if count(t.get("note"), "scope") >= 3:
        return False
    o._ensure_wake("architect", "plan_change", t["id"],
                   feature=t.get("feature_id"),
                   body={"task": t["id"], "files": paths, "why": why[:400]})
    _requeue(o, t, now, why, "scope", PLAN_WAIT_S)
    return True


def decisions_for(o, t):
    """`decision`/`answer` messages in the mailbox of the task's agent (an
    engineer's name is its task's, so every message there is about it)."""
    mail_for = getattr(o.broker, "mail_for", None)
    if mail_for is None:
        return []
    out = []
    for name in sorted({t.get("cur_name"), t.get("eng_name")} - {None}):
        try:
            msgs = mail_for(name)
        except Exception:  # noqa: BLE001 - the broker may be stopping
            continue
        out += [m for m in msgs if m.get("type") in DECISION_TYPES]
    return out


def _decided(o, t, why, now):
    if count(t.get("note"), "decided") >= 1:
        return False
    msgs = decisions_for(o, t)
    if not msgs:
        return False
    lines = []
    for m in msgs:
        body = m.get("body")
        text = body if isinstance(body, str) else json.dumps(body, sort_keys=True)
        lines.append(f"- {m.get('type')} from {m.get('from')}: {text[:600]}")
    _requeue(o, t, now, why, "decided", 0, note=(
        "You reported blocked on a question that was already answered in "
        "your mailbox; act on it:\n" + "\n".join(lines)))
    o.store.event(t["id"], "triage_decided", f"{len(msgs)} decision(s) in the mailbox")
    return True


def handle(o, t, role, why, data, now):
    cls = classify(role, why)
    if cls == "infra":
        _infra(o, t, why, now)
        return True
    if cls == "nocommit":
        return _nocommit(o, t, why, data, now)
    if cls == "scope" and t["kind"] == "work":
        return _scope(o, t, why, now)
    if t["kind"] == "work" and _decided(o, t, why, now):
        return True
    return False
