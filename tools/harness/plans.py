"""Validators for plan.json and assignments.json (ADR-0047 section 4a).

Pure functions: they take parsed JSON and return a list of error strings;
an empty list means valid. The schema files under planning/orchestrator/ are
the contract for the shape; a small subset validator (type, const, enum,
required, properties, additionalProperties, items, minItems, pattern) reads
them so there is one source of truth and no third-party dependency. The rules
a schema cannot say (overlap, ordering, acyclicity, reserved numbers) are
code below.
"""
import json
import os
import re

from .scheduler import files_overlap

SCHEMA_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))), "planning", "orchestrator")
KIND_RANK = {"contract": 0, "engine": 1, "ui": 2, "tests": 3, "docs": 4}
TRACK_ENGINEERS = {
    "daw": ("rt-engineer", "ui-engineer"),
    "plugin": ("dsp-engineer", "plugin-engineer"),
    "os": ("distro-engineer", "driver-engineer"),
}
TESTER = "tester"


def load_schema(name):
    with open(os.path.join(SCHEMA_DIR, name), encoding="utf-8") as f:
        return json.load(f)


_TYPES = {"string": str, "boolean": bool, "array": list, "object": dict,
          "null": type(None)}


def _is(value, tname):
    if tname == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if tname == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    return isinstance(value, _TYPES[tname])


def check_schema(schema, value, path="$"):
    errs = []
    if "const" in schema and value != schema["const"]:
        return [f"{path}: must be {schema['const']!r}"]
    if "enum" in schema and value not in schema["enum"]:
        return [f"{path}: {value!r} not one of {schema['enum']}"]
    if "type" in schema:
        want = schema["type"] if isinstance(schema["type"], list) \
            else [schema["type"]]
        if not any(_is(value, t) for t in want):
            return [f"{path}: expected {'/'.join(want)}"]
    if isinstance(value, str) and "pattern" in schema and \
            not re.search(schema["pattern"], value):
        errs.append(f"{path}: {value!r} does not match {schema['pattern']}")
    if isinstance(value, list):
        if len(value) < schema.get("minItems", 0):
            errs.append(f"{path}: needs at least {schema['minItems']} item(s)")
        if "items" in schema:
            for i, v in enumerate(value):
                errs += check_schema(schema["items"], v, f"{path}[{i}]")
    if isinstance(value, dict):
        props = schema.get("properties", {})
        for k in schema.get("required", ()):
            if k not in value:
                errs.append(f"{path}: missing {k}")
        for k, v in value.items():
            if k in props:
                errs += check_schema(props[k], v, f"{path}.{k}")
            elif schema.get("additionalProperties") is False:
                errs.append(f"{path}: unknown field {k}")
    return errs


def _cycle(deps):
    """A task id on a dependency cycle, or None."""
    state = {}

    def visit(n):
        if state.get(n) == 1:
            return n
        if state.get(n) == 2:
            return None
        state[n] = 1
        for d in deps.get(n, ()):
            if d in deps:
                hit = visit(d)
                if hit:
                    return hit
        state[n] = 2
        return None
    for n in deps:
        hit = visit(n)
        if hit:
            return hit
    return None


def _closure(deps, n):
    seen, todo = set(), list(deps.get(n, ()))
    while todo:
        d = todo.pop()
        if d not in seen:
            seen.add(d)
            todo += deps.get(d, ())
    return seen


def validate_plan(plan, expected=None, active=None):
    """expected: {"feature", "track", "adr", "protocol", "formats"} the
    daemon reserved for this feature. active: {feature id: [globs]} of every
    other live feature."""
    errs = check_schema(load_schema("plan.schema.json"), plan)
    if errs:
        return errs  # the rules below assume the shape
    tasks = plan["tasks"]
    ids = [t["id"] for t in tasks]
    for i in sorted({x for x in ids if ids.count(x) > 1}):
        errs.append(f"duplicate task id {i}")
    by = {t["id"]: t for t in tasks}
    deps = {t["id"]: list(t["depends_on"]) for t in tasks}

    expected = expected or {}
    for key in ("feature", "track"):
        if key in expected and plan[key] != expected[key]:
            errs.append(f"{key} is {plan[key]!r}, the request said "
                        f"{expected[key]!r}")
    if plan["feature_branch"] != f"feat/{plan['feature']}":
        errs.append(f"feature_branch must be feat/{plan['feature']}")
    res = plan["reserved"]
    # null means "this feature needs no such number"; a number must be the
    # one the daemon reserved, never one the architect picked.
    for key in ("adr", "protocol"):
        if res[key] is not None and res[key] != expected.get(key):
            errs.append(f"reserved.{key} is {res[key]!r}, the daemon "
                        f"reserved {expected.get(key)!r}")
    stray = sorted(set(res["formats"]) - set(expected.get("formats", ())))
    if stray:
        errs.append(f"reserved.formats {stray} were not reserved by the "
                    "daemon")
    if res["adr"] is not None:
        if not plan["adr"]:
            errs.append("reserved.adr is set but adr (the file) is null")
        elif f"ADR-{res['adr']:04d}" not in plan["adr"]:
            errs.append(f"adr path must contain ADR-{res['adr']:04d}")
    elif plan["adr"]:
        errs.append("adr names a file but no ADR number was reserved")
    if plan["pr"]["partial"] and not plan["pr"]["partial_reason"]:
        errs.append("pr.partial needs a partial_reason")

    for t in tasks:
        for d in t["depends_on"]:
            if d not in by:
                errs.append(f"{t['id']}: depends_on unknown task {d}")
            elif d == t["id"]:
                errs.append(f"{t['id']}: depends on itself")
    hit = _cycle(deps)
    if hit:
        errs.append(f"depends_on has a cycle through {hit}")
        return errs

    contracts = [t["id"] for t in tasks if t["kind"] == "contract"]
    for t in tasks:
        if t["kind"] == "contract" and t["depends_on"]:
            errs.append(f"{t['id']}: a contract task has no dependencies")
        for d in t["depends_on"]:
            if d in by and KIND_RANK[by[d]["kind"]] > KIND_RANK[t["kind"]]:
                errs.append(f"{t['id']} ({t['kind']}) depends on {d} "
                            f"({by[d]['kind']}): contract, engine, ui, tests, "
                            "docs is the order")
        if t["kind"] != "contract":
            missing = [c for c in contracts
                       if c not in _closure(deps, t["id"])]
            if missing:
                errs.append(f"{t['id']}: must depend on contract task "
                            f"{', '.join(missing)} (contract first)")

    order = plan["merge_order"]
    if sorted(order) != sorted(ids) or len(set(order)) != len(order):
        errs.append("merge_order must list every task exactly once")
    else:
        pos = {n: i for i, n in enumerate(order)}
        for t in tasks:
            for d in t["depends_on"]:
                if pos[d] > pos[t["id"]]:
                    errs.append(f"merge_order puts {t['id']} before its "
                                f"dependency {d}")
        for c in contracts:
            for t in tasks:
                if t["kind"] != "contract" and pos[t["id"]] < pos[c]:
                    errs.append(f"merge_order puts {t['id']} before "
                                f"contract task {c}")

    for i, a in enumerate(tasks):
        for b in tasks[i + 1:]:
            if files_overlap(a["files"], b["files"]):
                errs.append(f"files of {a['id']} and {b['id']} overlap")
    for fid, globs in sorted((active or {}).items()):
        for t in tasks:
            if files_overlap(t["files"], globs):
                errs.append(f"files of {t['id']} overlap active feature {fid}")
    return errs


def sub_branch(feature, task):
    """Not feat/<feature>/<task>: git cannot hold refs/heads/feat/f and
    refs/heads/feat/f/t at once."""
    return f"sub/{feature}/{task}"


def validate_assignments(table, plan):
    errs = check_schema(load_schema("assignments.schema.json"), table)
    if errs:
        return errs
    if table["feature"] != plan["feature"]:
        return [f"feature is {table['feature']!r}, the plan is "
                f"{plan['feature']!r}"]
    by = {t["id"]: t for t in plan["tasks"]}
    rows = {}
    for r in table["rows"]:
        if r["task"] in rows:
            errs.append(f"two rows for task {r['task']}")
        rows[r["task"]] = r
    for tid in by:
        if tid not in rows:
            errs.append(f"no row for task {tid}")
    for tid in rows:
        if tid not in by:
            errs.append(f"row for unknown task {tid}")
    if errs:
        return errs
    orders = [r["order"] for r in rows.values()]
    if len(set(orders)) != len(orders) or min(orders) < 1:
        errs.append("order must be unique and start at 1 or more")
    ok_agents = set(TRACK_ENGINEERS[plan["track"]])
    for tid, r in sorted(rows.items()):
        t = by[tid]
        if r["engineer"] not in ok_agents | {t["agent"]}:
            errs.append(f"{tid}: {r['engineer']} is not an engineer of "
                        f"track {plan['track']}")
        if r["sub_branch"] != sub_branch(plan["feature"], tid):
            errs.append(f"{tid}: sub_branch must be "
                        f"{sub_branch(plan['feature'], tid)}")
        allowed = {plan["feature_branch"]} if not t["depends_on"] else {
            sub_branch(plan["feature"], d) for d in t["depends_on"]}
        if r["start_from"] not in allowed:
            errs.append(f"{tid}: start_from must be one of "
                        f"{sorted(allowed)}")
        for d in t["depends_on"]:
            if rows[d]["order"] >= r["order"]:
                errs.append(f"{tid}: order must come after its dependency "
                            f"{d}")
    return errs
