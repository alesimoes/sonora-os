#!/usr/bin/env python3
"""The micro build's job summary (GITHUB_STEP_SUMMARY): the changed files,
the targets built and the tests run with their result and time, as
markdown on stdout.

    micro_summary.py DIR [TITLE]

DIR is .micro-build/: `files` (changed paths), `plan.json` (micro_targets.py
plan) and, once the tests ran, `ctest.xml` (ctest --output-junit)."""
import json
import os
import sys
import xml.etree.ElementTree as ET

MAX_ROWS = 40


def read_lines(path):
    try:
        with open(path, encoding="utf-8") as f:
            return [ln.strip() for ln in f if ln.strip()]
    except OSError:
        return []


def read_plan(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def read_results(path):
    """[(name, status, seconds)] from a ctest JUnit file; [] if absent."""
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError):
        return []
    out = []
    for case in root.iter("testcase"):
        status = (case.get("status") or "").lower()
        if case.find("failure") is not None or status == "fail":
            word = "FAIL"
        elif case.find("skipped") is not None or status == "notrun":
            word = "skipped"
        else:
            word = "pass"
        out.append((case.get("name", "?"), word, float(case.get("time") or 0)))
    return out


def clip(rows):
    if len(rows) <= MAX_ROWS:
        return rows, 0
    return rows[:MAX_ROWS], len(rows) - MAX_ROWS


def render(title, files, plan, results):
    lines = [f"### {title}", ""]
    if plan is None:
        lines.append("No plan was written (nothing to build, or the "
                     "configure step failed).")
        return "\n".join(lines) + "\n"
    targets = plan.get("targets") or []
    tests = plan.get("tests")
    ntests = len(tests) if isinstance(tests, list) else "all"
    lines.append(f"{len(files)} changed files, scope `{plan.get('scope')}`, "
                 f"{len(targets)} targets built, {ntests} tests run.")
    lines.append("")
    failed = [r for r in results if r[1] == "FAIL"]
    if results:
        lines.append(f"Tests: {len(results) - len(failed)} passed, "
                     f"{len(failed)} failed, "
                     f"{sum(r[2] for r in results):.1f} s of test time.")
        lines.append("")
    lines += ["| Changed file | Reaches |", "|---|---|"]
    why = plan.get("why") or {}
    shown, more = clip(sorted(why.items()))
    lines += [f"| `{k}` | {v[:120]} |" for k, v in shown]
    if more:
        lines.append(f"| ... | {more} more |")
    if results:
        lines += ["", "| Test | Result | Seconds |", "|---|---|---|"]
        ordered = sorted(results, key=lambda r: (r[1] != "FAIL", -r[2]))
        shown, more = clip(ordered)
        lines += [f"| {n} | {s} | {t:.2f} |" for n, s, t in shown]
        if more:
            lines.append(f"| ... | {more} more passing | |")
    if targets:
        lines += ["", "<details><summary>Targets built</summary>", "",
                  ", ".join(f"`{t}`" for t in targets), "", "</details>"]
    return "\n".join(lines) + "\n"


def main(argv):
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    d = argv[1]
    title = argv[2] if len(argv) > 2 else "Micro build"
    sys.stdout.write(render(title, read_lines(os.path.join(d, "files")),
                            read_plan(os.path.join(d, "plan.json")),
                            read_results(os.path.join(d, "ctest.xml"))))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
