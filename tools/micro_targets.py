#!/usr/bin/env python3
"""Which CMake targets and ctest tests a set of changed files affects
(ADR-0061, micro builds).

Reads the CMake file API reply of a configured build tree (codemodel-v2:
every target, its sources, its source directory, its dependencies and its
artifacts) and `ctest --show-only=json-v1` (every test and the command it
runs), and answers: from these changed paths, which targets must be built
and which tests run so that "what was written compiles and its tests pass"
is known, without building the tree.

    tools/micro_targets.py query BUILD_DIR
        write the file API query; run before `cmake -S . -B BUILD_DIR`
    tools/micro_targets.py plan BUILD_DIR SOURCE_ROOT [--files-from F] [PATH...]
        print the plan as JSON: {"scope": "none|targets|all", "targets": [],
        "tests": [], "why": {path: reason}}

Owning a path: a target whose sources list the file; else, for a header, a
CMakeLists.txt or any other file under a target's source directory, every
target of that directory (the header is not in the source list, but every
unit of the directory may include it). Dependents are followed transitively:
a header of common/base changes what engine/core compiles against, so the
gate must compile engine/core too (the price of a shared header, listed in
ADR-0061 as the boundary to split). A path no target owns and no build file
touches (docs, the harness, the dashboard) affects nothing; a path that is a
build file above every target (the root CMakeLists.txt, tools/*.cmake) is
"all". A test is run when any word of its command is an artifact of an
affected target (the engine-process tests name sonora-engine by path).
"""
import argparse
import glob
import json
import os
import re
import sys

CLIENT = "client-sonora-micro"
ROOT_BUILD_FILES = ("CMakeLists.txt", "CMakePresets.json")
ROOT_BUILD_DIRS = ("cmake/",)


def write_query(build):
    q = os.path.join(build, ".cmake", "api", "v1", "query", CLIENT)
    os.makedirs(q, exist_ok=True)
    with open(os.path.join(q, "query.json"), "w") as f:
        json.dump({"requests": [{"kind": "codemodel", "version": 2}]}, f)


def _load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def read_codemodel(build):
    """[(target dict)] from the reply, each with name, id, type, sources
    (source-root-relative, '/' separated), dir (source-root-relative
    directory), deps (ids) and artifacts (build-relative paths)."""
    reply = os.path.join(build, ".cmake", "api", "v1", "reply")
    idx = sorted(glob.glob(os.path.join(reply, "index-*.json")))
    if not idx:
        raise SystemExit(f"micro_targets: no file API reply under {reply}; "
                         "run `query` before cmake configures")
    index = _load(idx[-1])
    cm = index["reply"][CLIENT]["query.json"]["responses"][0]["jsonFile"]
    model = _load(os.path.join(reply, cm))
    source_root = model["paths"]["source"]
    conf = model["configurations"][0]
    out = []
    for t in conf["targets"]:
        tj = _load(os.path.join(reply, t["jsonFile"]))
        tdir = tj.get("paths", {}).get("source", "")
        out.append({
            "name": tj["name"], "id": tj["id"], "type": tj.get("type", ""),
            "dir": norm(tdir, source_root),
            "sources": [norm(s["path"], source_root)
                        for s in tj.get("sources", [])],
            "deps": [d["id"] for d in tj.get("dependencies", [])],
            "artifacts": [a["path"] for a in tj.get("artifacts", [])],
        })
    return out, source_root


def norm(path, source_root):
    """A path as the repository names it: relative to the source root,
    forward slashes, no leading './'. Absolute paths outside the root stay
    absolute (a generated file in the build tree)."""
    p = path.replace("\\", "/")
    root = source_root.replace("\\", "/").rstrip("/") + "/"
    if p.startswith(root):
        p = p[len(root):]
    elif p.startswith("/") or (len(p) > 1 and p[1] == ":"):
        return p
    while p.startswith("./"):
        p = p[2:]
    return p.rstrip("/") if p != "." else ""


def owners(path, targets):
    """Targets that own `path`: by source list first, then by directory
    (the deepest directory wins: a file of engine/core/tests belongs to the
    test targets defined there, not to every target under engine/core)."""
    by_source = [t for t in targets if path in t["sources"]]
    if by_source:
        return by_source
    best, depth = [], -1
    for t in targets:
        d = t["dir"]
        if d == "" or path == d or path.startswith(d + "/"):
            n = d.count("/") + 1 if d else 0
            if n > depth:
                best, depth = [t], n
            elif n == depth:
                best.append(t)
    # A root-directory target (dir == "") owns nothing by directory: that
    # would make every stray file a change to the whole tree. Root build
    # files are handled by is_root_build_file.
    return [t for t in best if t["dir"] != ""]


def is_root_build_file(path):
    return path in ROOT_BUILD_FILES or path.startswith(ROOT_BUILD_DIRS) \
        or (path.startswith("tools/") and path.endswith(".cmake"))


def dependents_closure(seed_ids, targets, direct=False):
    rev = {}
    for t in targets:
        for d in t["deps"]:
            rev.setdefault(d, set()).add(t["id"])
    out, frontier = set(seed_ids), set(seed_ids)
    while frontier:
        nxt = set()
        for tid in frontier:
            nxt |= rev.get(tid, set()) - out
        out |= nxt
        frontier = nxt
        if direct:
            break
    return out


def plan(files, targets, direct=False):
    why, seed = {}, set()
    scope = "none"
    for f in files:
        f = norm(f, "")
        if not f:
            continue
        if is_root_build_file(f):
            why[f] = "root build file: whole tree"
            scope = "all"
            continue
        own = owners(f, targets)
        if own:
            seed |= {t["id"] for t in own}
            why[f] = "owned by " + ", ".join(sorted(t["name"] for t in own))
        else:
            why[f] = "no target: nothing to build"
    if scope == "all":
        return {"scope": "all", "targets": sorted(t["name"] for t in targets
                                                  if buildable(t)),
                "tests": None, "why": why}
    if not seed:
        return {"scope": "none", "targets": [], "tests": [], "why": why}
    ids = dependents_closure(seed, targets, direct)
    by_id = {t["id"]: t for t in targets}
    chosen = [by_id[i] for i in ids if i in by_id and buildable(by_id[i])]
    return {"scope": "targets",
            "targets": sorted(t["name"] for t in chosen),
            "tests": None, "why": why, "_ids": sorted(ids)}


def buildable(t):
    # No UTILITY: an aggregate such as the portable target depends on every
    # neutral test, and "building" it would build them all.
    return t["type"] in ("EXECUTABLE", "STATIC_LIBRARY", "SHARED_LIBRARY",
                         "MODULE_LIBRARY", "OBJECT_LIBRARY")


ADD_TEST = re.compile(r"add_test\(\s*(\[=\[.*?\]=\]|[^\s()]+)\s*(.*?)\)[ \t]*\n",
                      re.S)
ARG = re.compile(r'"((?:[^"\\]|\\.)*)"|\[=\[(.*?)\]=\]|(\S+)')


def ctest_listing(build):
    """{"tests": [{"name", "command": [...]}]} from the CTestTestfile.cmake
    files cmake generated, in the shape of `ctest --show-only=json-v1`.
    Read from the files, not from ctest: before the build ctest leaves out
    the command of a test whose executable does not exist yet, which is
    every test the plan is about to build."""
    tests = []
    for dirpath, _, files in os.walk(build):
        if "CTestTestfile.cmake" not in files:
            continue
        with open(os.path.join(dirpath, "CTestTestfile.cmake"),
                  encoding="utf-8", errors="replace") as f:
            text = f.read()
        for m in ADD_TEST.finditer(text):
            name = m.group(1)
            if name.startswith("[=["):
                name = name[3:-3]
            cmd = [a or b or c for a, b, c in ARG.findall(m.group(2))]
            tests.append({"name": name, "command": cmd})
    return {"tests": tests}


def match_tests(ctest_json, targets, names):
    want = set(names)
    arts = set()
    for t in targets:
        if t["name"] in want:
            for a in t["artifacts"]:
                arts.add(os.path.basename(a.replace("\\", "/")))
    found = []
    for test in ctest_json.get("tests", []):
        cmd = test.get("command") or []
        words = []
        for w in cmd:
            words.append(os.path.basename(str(w).replace("\\", "/")))
            # "build/sonora-engine" inside a shell -c string or an option
            # value (--engine build/sonora-engine) counts too.
            words += [os.path.basename(x) for x in str(w).split()]
        if any(w in arts for w in words):
            found.append(test["name"])
    return sorted(set(found))


def prune_apps(targets, names, ctest_json):
    """Libraries and test executables only (owner, 2026-11-07: a PR gate
    checks that the code works, it does not produce the product). An
    executable stays when a ctest test runs it (the test binaries, and
    sonora-engine for the engine-process tests); sonora-ui, sonora-analyze
    and the like are linked by the daily full run and the release."""
    by_name = {t["name"]: t for t in targets}
    run = set()
    for test in ctest_json.get("tests", []):
        for w in test.get("command") or []:
            for x in [str(w)] + str(w).split():
                run.add(os.path.basename(x.replace("\\", "/")))
    keep = []
    for n in names:
        t = by_name.get(n)
        if t and t["type"] == "EXECUTABLE" and not any(
                os.path.basename(a.replace("\\", "/")) in run
                for a in t["artifacts"]):
            continue
        keep.append(n)
    return keep


def ctest_regex(tests):
    return "^(" + "|".join(re.escape(t) for t in tests) + ")$"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    q = sub.add_parser("query")
    q.add_argument("build")
    p = sub.add_parser("plan")
    p.add_argument("build")
    p.add_argument("source_root")
    p.add_argument("--files-from")
    p.add_argument("--direct", action="store_true",
                   help="direct dependents only (default: transitive)")
    p.add_argument("--no-tests", action="store_true")
    p.add_argument("paths", nargs="*")
    a = ap.parse_args(argv)
    if a.cmd == "query":
        write_query(a.build)
        return 0
    files = list(a.paths)
    if a.files_from:
        with open(a.files_from, encoding="utf-8") as f:
            files += [ln.strip() for ln in f if ln.strip()]
    targets, _ = read_codemodel(a.build)
    out = plan(files, targets, a.direct)
    out.pop("_ids", None)
    if out["scope"] == "all":
        out["tests"] = None if a.no_tests else "all"
    elif out["scope"] == "targets" and not a.no_tests:
        listing = ctest_listing(a.build)
        out["targets"] = prune_apps(targets, out["targets"], listing)
        out["tests"] = match_tests(listing, targets, out["targets"])
        out["ctest_regex"] = ctest_regex(out["tests"]) if out["tests"] else ""
    json.dump(out, sys.stdout, indent=1, sort_keys=True)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
