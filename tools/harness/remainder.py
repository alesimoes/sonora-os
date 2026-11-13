"""The rest of a partially merged feature gets back into the queue (2026-11-13).

A feature merged with `partial: true` kept its id, and Flow.on_proposal
rejected any proposal of an existing id ("exists: merged"). 45 features had
merged as `[partial]` and nothing could plan what they left open: the
approved four-slice plan of ui.windows_nightly_tests was re-planned to one
task, merged as #674, and the other three slices had no task anywhere, while
the pools sat idle with "no eligible task".

The remainder is a feature of its own with the same BACKLOG ids:
`<root>.rest`, then `<root>.rest2`, ... up to MAX_ROUNDS, so ids stay valid
branch, worktree and progress names and the root's history is untouched.

  * offered: the lead's pool_under_floor prompt lists the chains whose tip
    merged partial (pool_prompt.py); a proposal naming the root or any link
    opens the next one (target/open_rest). Nothing opens on its own.
  * planned: the architect's brief names the PRs that merged the earlier
    parts and asks for ONLY what is still open (plan_lines). An empty plan
    completes the chain (ingest); a block with `needs_owner` goes to
    waits_on_owner like any wake (Orchestrator._block, owner_infer).
  * settled: a link that merges not partial clears `partial` on the whole
    chain (settle, on Flow.tick); a link that merges partial is offered
    again until MAX_ROUNDS, then the chain is listed as spent for the owner.
"""
import json
import re

from . import closed, queueimport

SUFFIX = ".rest"
MAX_ROUNDS = 3
MAX_ID = 48     # Flow.open_feature's id rule
_REST = re.compile(r"^(?P<root>.+?)\.rest(?P<n>[2-9]?)$")
TERMINAL = ("merged", "closed")
EMPTY_WHY = "remainder: nothing left to build"


def root_of(fid):
    m = _REST.match(fid or "")
    return m.group("root") if m else fid


def round_of(fid):
    """0 for the root, 1 for `.rest`, n for `.restN`."""
    m = _REST.match(fid or "")
    return int(m.group("n") or 1) if m else 0


def rest_id(root, n):
    return f"{root}{SUFFIX}{n if n > 1 else ''}"


def chain(store, root):
    """The root and its existing remainders, in order."""
    out = []
    for n in range(MAX_ROUNDS + 1):
        f = store.get_feature(rest_id(root, n) if n else root)
        if f is None:
            break
        out.append(f)
    return out


def _info(store, fid):
    try:
        return json.loads(store.get_meta(f"feature:{fid}", "{}"))
    except ValueError:
        return {}


def _open_link(f):
    """A link that still has work, or waits on someone: no next one yet."""
    return f["state"] not in TERMINAL


def next_id(store, fid):
    """The id the chain of fid continues with, or None."""
    links = chain(store, root_of(fid))
    if not links or not links[0]["partial"] or links[0]["state"] != "merged":
        return None
    tip = links[-1]
    if _open_link(tip) or (tip["state"] == "merged" and not tip["partial"]):
        return None
    n = round_of(tip["id"]) + 1
    return rest_id(links[0]["id"], n) if n <= MAX_ROUNDS else None


def merged_pr(store, f):
    """The PR that merged feature f: its own, or a train entry that carried
    it (its branch, one of its slice branches, or a resolver's branch)."""
    if f.get("pr"):
        return f["pr"]
    fid = f["id"]
    for e in store.features():
        b = e.get("branch") or ""
        if e["id"] != f"pr-{e.get('pr')}" or e["state"] != "merged":
            continue
        if b == f.get("branch") or b.startswith(f"sub/{fid}/") \
                or store.get_meta("carried_branch:" + b) == fid:
            return e["pr"]
    return None


def prs(store, links):
    return [p for p in (merged_pr(store, f) for f in links
                        if f["state"] == "merged") if p]


def _in_scope(store, f):
    ids = _info(store, f["id"]).get("backlog_ids", [])
    return not queueimport.out_of_scope(" ".join([f["id"], *map(str, ids)]))


def continuable(store, track=None):
    """[{root, next, track, prs}] for the lead, oldest first PR first."""
    rows = []
    for f in store.features():
        if round_of(f["id"]) or (track and f["track"] != track) \
                or not _in_scope(store, f):
            continue
        nid = next_id(store, f["id"])
        if nid:
            rows.append({"root": f["id"], "next": nid, "track": f["track"],
                         "prs": prs(store, chain(store, f["id"]))})
    return sorted(rows, key=lambda r: (r["prs"] or [10 ** 9])[0])


def spent(store, track=None):
    """Roots still partial whose chain used every round: the owner's call."""
    out = []
    for f in store.features():
        if round_of(f["id"]) or not f["partial"] or f["state"] != "merged" \
                or (track and f["track"] != track):
            continue
        links = chain(store, f["id"])
        if len(links) > MAX_ROUNDS and next_id(store, f["id"]) is None \
                and not _open_link(links[-1]):
            out.append(f["id"])
    return out


def target(store, fid):
    """(remainder id, None) when a proposal continues a chain; (None, why)
    when it names a remainder that cannot be opened; (None, None) when it is
    not about a remainder at all."""
    root = root_of(fid)
    if fid == root and store.get_feature(fid) is None:
        return None, None
    if store.get_feature(root) is None:
        return None, f"no feature {root} to continue"
    nid = next_id(store, fid)
    if nid and len(nid) > MAX_ID:
        return None, f"{nid} is longer than a feature id may be"
    if nid:
        return nid, None
    if store.get_feature(fid) is not None:
        return None, None       # the usual "exists" rejection says it
    return None, f"{root} has nothing to continue"


def open_rest(flow, rid, sender):
    """Open remainder rid as a queued feature. True when it did."""
    store = flow.store
    links = chain(store, root_of(rid))
    root, tip = links[0], links[-1]
    info = _info(store, root["id"])
    made = flow.open_feature(
        rid, root["track"], info.get("backlog_ids") or [root["id"]],
        brief=f"remainder of {root['id']}", wants=())
    if made:
        info = _info(store, rid)
        info.update({"continues": tip["id"], "root": root["id"],
                     "round": round_of(rid), "prs": prs(store, links),
                     "root_adr": root.get("adr_no")})
        store.set_meta(f"feature:{rid}", json.dumps(info))
    store.event(None, "proposal_accepted" if made else "proposal_rejected",
                f"{sender}: {rid}" + ("" if made else " (exists)"))
    return made


def is_rest(flow, fid):
    return bool(_info(flow.store, fid).get("continues"))


def plan_lines(flow, fid):
    """The architect's brief for a remainder; [] for any other feature."""
    info = _info(flow.store, fid)
    if not info.get("continues"):
        return []
    root, prev = info["root"], info["continues"]
    nums = ", ".join(f"#{p}" for p in info.get("prs", [])) or "(PR unknown)"
    reads = sorted({f"planning/plans/{root}.md", f"planning/plans/{prev}.md"})
    adr = info.get("root_adr")
    return [
        f"REMAINDER (round {info.get('round')} of at most {MAX_ROUNDS}): "
        f"{root} merged partially, its earlier part(s) as PR {nums}.",
        f"Read those PRs' bodies (`gh pr view N`) and {', '.join(reads)}; "
        "plan ONLY what is still open, never what already merged.",
        f"Nothing left: write {flow.plan_file(fid)} with \"tasks\": [] and "
        "the feature is marked complete (partial false).",
        "What is left needs hardware or an owner decision: finish with "
        "status blocked and `needs_owner` saying what the owner must decide; "
        "it goes to waits_on_owner.",
        "No new ADR number is reserved for a remainder: set adr and "
        f"reserved.adr to null and amend ADR-{adr:04d} as a file of a task."
        if adr else "No ADR number is reserved for a remainder: adr and "
        "reserved.adr are null."]


def complete(store, fid):
    """The chain of fid is done: the root and every link stop being
    partial. Returns the ids it changed."""
    changed = []
    for f in chain(store, root_of(fid)):
        if f["partial"]:
            store.set_feature(f["id"], partial=0)
            changed.append(f["id"])
    if changed:
        store.event(None, "remainder_complete",
                    f"{root_of(fid)}: {', '.join(changed)}")
    return changed


def ingest(flow, fid):
    """A remainder's first plan: an empty one completes the chain; every
    other plan is validated like any feature's. (ok, errors)"""
    if not is_rest(flow, fid):
        return flow.ingest_plan(fid)
    plan, err = flow._read_json(flow.plan_file(fid))
    if err is None and isinstance(plan, dict) and plan.get("tasks") == []:
        complete(flow.store, fid)
        closed.close_feature(flow.o, flow.store.get_feature(fid), EMPTY_WHY)
        return True, []
    return flow.ingest_plan(fid)


def settle(store):
    """Tick: a remainder that merged not partial completes its chain.
    Idempotent: it acts only while the root is still partial."""
    for f in store.features():
        if round_of(f["id"]) and f["state"] == "merged" and not f["partial"]:
            root = store.get_feature(root_of(f["id"]))
            if root is not None and root["partial"]:
                complete(store, f["id"])
