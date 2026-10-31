#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""How far along SonoraOS is, from planning/progress.json.

    tools/progress.py              terminal report
    tools/progress.py --ascii      same, without Unicode block characters
    tools/progress.py --markdown   regenerate planning/PROGRESS.md
    tools/progress.py --check      fail if the catalogue is invalid (PR CI runs this)
    tools/progress.py --check-generated
                                   also fail if PROGRESS.md is stale (local use)
    tools/progress.py --summary    one line, for hooks
    tools/progress.py --resolve-this-pr N
                                   rewrite "this PR" in evidence to "PR #N"
    tools/progress.py --pr-from-subject SUBJECT
                                   print N from a squash subject "... (#N)"
    tools/progress.py get ID       one item, in a few lines
    tools/progress.py list [--open] [--track T]
                                   one line per item; T is a module id
                                   (daw.plugins or plugins) or an owner
                                   (plugin-engineer)
    tools/progress.py set ID STATE [--evidence TEXT] [--pr N] [--branch B]
                                   edit one item in place; --pr N makes the
                                   evidence "PR #N: TEXT"

Agents read and set progress through get, list and set: the catalogue is too
large to read whole, and set rewrites only the item's own line.

PROGRESS.md is regenerated on main by CI after each merge and never in a
PR: a generated file edited by every PR made each merge conflict with every
queued PR.

A PR cannot know its own number before it is opened, so its evidence says
"this PR". The same CI job on main resolves that to "PR #N" from the squash
commit's "(#N)" suffix, so nobody has to bounce a PR to fill it in.

Only merged and verified work counts as done. Work in progress is reported
next to the percentage and never folded into it, so the number cannot be
raised by opening branches.
"""

import argparse
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CATALOGUE = os.path.join(ROOT, "planning", "progress.json")
REPORT = os.path.join(ROOT, "planning", "PROGRESS.md")
STATUSES = ("todo", "doing", "done")
BAR_WIDTH = 20


class Invalid(Exception):
    pass


def load(path=CATALOGUE):
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError) as e:
        raise Invalid("cannot read %s: %s" % (path, e))
    validate(data)
    return data


def validate(data):
    errors = []
    sizes = data.get("sizes")
    phases = data.get("phases")
    if not isinstance(sizes, dict) or not sizes:
        errors.append("'sizes' must map size names to points")
        sizes = {}
    if not isinstance(phases, dict) or not phases:
        errors.append("'phases' must map phase numbers to names")
        phases = {}
    seen = set()

    def unique(kind, ident):
        if not isinstance(ident, str) or not ident:
            errors.append("%s without an id" % kind)
        elif ident in seen:
            errors.append("duplicate id %r" % ident)
        else:
            seen.add(ident)

    fronts = data.get("fronts")
    if not isinstance(fronts, list) or not fronts:
        errors.append("'fronts' must be a non-empty list")
        fronts = []
    for front in fronts:
        unique("front", front.get("id"))
        for module in front.get("modules") or []:
            unique("module", module.get("id"))
            if not module.get("items"):
                errors.append("module %r has no items" % module.get("id"))
            for item in module.get("items") or []:
                ident = item.get("id")
                unique("item", ident)
                where = "item %r" % ident
                if not item.get("title"):
                    errors.append("%s has no title" % where)
                if item.get("size") not in sizes:
                    errors.append("%s: size %r is not one of %s"
                                  % (where, item.get("size"), sorted(sizes)))
                if str(item.get("phase")) not in phases:
                    errors.append("%s: phase %r is not one of %s"
                                  % (where, item.get("phase"), sorted(phases)))
                status = item.get("status")
                if status not in STATUSES:
                    errors.append("%s: unknown status %r" % (where, status))
                if status == "done" and not str(item.get("evidence", "")).strip():
                    errors.append("%s is done without evidence" % where)
                if status == "doing" and not str(item.get("branch", "")).strip():
                    errors.append("%s is doing without a branch" % where)
    if errors:
        raise Invalid("\n".join(errors))


THIS_PR = re.compile(r"\bthis PR\b")
# Evidence values in the raw JSON text, so the rewrite keeps the file's
# hand-kept layout instead of re-serialising it.
EVIDENCE = re.compile(r'("evidence"\s*:\s*")((?:[^"\\]|\\.)*)(")')
SQUASH_SUFFIX = re.compile(r"\(#(\d+)\)\s*$")


def resolve_this_pr(text, number):
    """Returns (text, count) with "this PR" in evidence values made "PR #N".

    >>> resolve_this_pr('{"evidence": "this PR: ctest 3/3"}', 33)
    ('{"evidence": "PR #33: ctest 3/3"}', 1)
    >>> resolve_this_pr('{"evidence": "see this PR and this PR."}', 7)
    ('{"evidence": "see PR #7 and PR #7."}', 2)
    >>> resolve_this_pr('{"evidence": "this PRs, unthis PR, this pr"}', 7)[1]
    0
    >>> resolve_this_pr('{"title": "this PR", "evidence": "PR #3"}', 7)[1]
    0
    """
    count = 0

    def fix(m):
        nonlocal count
        value, n = THIS_PR.subn("PR #%d" % number, m.group(2))
        count += n
        return m.group(1) + value + m.group(3)

    return EVIDENCE.sub(fix, text), count


def pr_from_subject(subject):
    """The PR number from a squash-merge subject, or None.

    >>> pr_from_subject("ADR-0007 M4: control API (contract/api) (#33)")
    33
    >>> pr_from_subject("Regenerate PROGRESS.md [skip ci]") is None
    True
    >>> pr_from_subject("Fix (#12) in the middle of a subject") is None
    True
    """
    m = SQUASH_SUFFIX.search(subject)
    return int(m.group(1)) if m else None


def find_item(data, ident):
    """(front, module, item) for an item id, else None."""
    for front in data["fronts"]:
        for module in front["modules"]:
            for item in module["items"]:
                if item["id"] == ident:
                    return front, module, item
    return None


STRING = r'"(?:[^"\\]|\\.)*"'


def set_field(line, key, value, after):
    """Sets "key": value in an item's line, else inserts the pair after the
    first key in `after` that the line has."""
    encoded = json.dumps(value, ensure_ascii=False)
    pair = re.compile(r'("%s": )%s' % (key, STRING))
    if pair.search(line):
        return pair.sub(lambda m: m.group(1) + encoded, line, count=1)
    for prev in after:
        m = re.search(r'"%s": %s' % (prev, STRING), line)
        if m:
            return '%s, "%s": %s%s' % (line[:m.end()], key, encoded,
                                       line[m.end():])
    raise Invalid("no place for %r in %s" % (key, line.strip()))


def set_item(text, ident, status, evidence=None, branch=None):
    r"""Returns text with one item's status, evidence and branch set and
    every other byte as it was, so the hand-kept layout survives. Raises
    Invalid for an unknown id or status, or a result --check would refuse.

    >>> doc = ('{"sizes": {"S": 1}, "phases": {"1": "One"}, "fronts": ['
    ...        '{"id": "f", "modules": [{"id": "f.m", "items": [\n'
    ...        '  {"id": "a", "title": "A", "size": "S", "phase": 1, "status": "todo", "note": "n"},\n'
    ...        '  {"id": "b", "title": "B", "size": "S", "phase": 1, "status": "done", "evidence": "PR #1"}\n'
    ...        ']}]}]}')
    >>> print(set_item(doc, "a", "doing", branch="t1-a").splitlines()[1])
      {"id": "a", "title": "A", "size": "S", "phase": 1, "status": "doing", "branch": "t1-a", "note": "n"},
    >>> print(set_item(doc, "a", "done", evidence='this PR: "x" 3/3').splitlines()[1])
      {"id": "a", "title": "A", "size": "S", "phase": 1, "status": "done", "evidence": "this PR: \"x\" 3/3", "note": "n"},
    >>> print(set_item(doc, "b", "done", evidence="PR #2").splitlines()[2])
      {"id": "b", "title": "B", "size": "S", "phase": 1, "status": "done", "evidence": "PR #2"}
    >>> set_item(doc, "b", "done") == doc
    True
    >>> set_item(doc, "a", "done")
    Traceback (most recent call last):
    progress.Invalid: item 'a' is done without evidence
    >>> set_item(doc, "c", "todo")
    Traceback (most recent call last):
    progress.Invalid: unknown item id 'c'
    >>> set_item(doc, "f.m", "todo")
    Traceback (most recent call last):
    progress.Invalid: unknown item id 'f.m'
    >>> split = doc.replace('"status": "todo", "note"', '"status": "todo",\n   "note"')
    >>> set_item(split, "a", "done", evidence="x")
    Traceback (most recent call last):
    progress.Invalid: item 'a' is not on a line of its own; edit it by hand
    >>> set_item(doc, "a", "finished")
    Traceback (most recent call last):
    progress.Invalid: unknown status 'finished'; one of todo, doing, done
    """
    if status not in STATUSES:
        raise Invalid("unknown status %r; one of %s"
                      % (status, ", ".join(STATUSES)))
    if find_item(json.loads(text), ident) is None:
        raise Invalid("unknown item id %r" % ident)
    line_of = re.compile(r'^[ \t]*\{"id": %s, .*$'
                         % re.escape(json.dumps(ident)), re.MULTILINE)
    matches = list(line_of.finditer(text))
    if len(matches) != 1:
        raise Invalid("item %r is not on a line of its own; edit it by hand"
                      % ident)
    m = matches[0]
    # An item spread over lines would get a second key from set_field, which
    # json.loads accepts silently (the last one wins).
    if not re.search(r"\},?[ \t\r]*$", m.group(0)):
        raise Invalid("item %r is not on a line of its own; edit it by hand"
                      % ident)
    line = set_field(m.group(0), "status", status, ())
    if branch is not None:
        line = set_field(line, "branch", branch, ("status",))
    if evidence is not None:
        line = set_field(line, "evidence", evidence, ("branch", "status"))
    out = text[:m.start()] + line + text[m.end():]
    validate(json.loads(out))
    return out


def on_track(module, track):
    """A module is on a track named by its id, the id without its front,
    or one of its owners ("rt-engineer (model squad)" is rt-engineer)."""
    t = track.lower()
    ident = module["id"].lower()
    owners = [o.split()[0] for o in
              str(module.get("owner", "")).lower().split(",") if o.split()]
    return t in (ident, ident.split(".", 1)[-1]) or t in owners


def render_item(data, ident):
    found = find_item(data, ident)
    if found is None:
        raise Invalid("unknown item id %r" % ident)
    front, module, item = found
    out = ["%s: %s" % (item["id"], item["title"]),
           "status %s, size %s, phase %s; %s / %s (%s)" % (
               item["status"], item["size"], item["phase"], front["title"],
               module["title"], module.get("owner", "no owner"))]
    out += ["%s: %s" % (key, item[key])
            for key in ("branch", "evidence", "ref", "note") if item.get(key)]
    return "\n".join(out) + "\n"


def render_list(data, open_only, track):
    out = []
    for front in data["fronts"]:
        for module in front["modules"]:
            if track and not on_track(module, track):
                continue
            for item in module["items"]:
                if open_only and item["status"] == "done":
                    continue
                title = item["title"]
                if len(title) > 72:
                    title = title[:69] + "..."
                out.append("%-34s %-5s %s  %s" % (
                    item["id"], item["status"], item["size"], title))
    return "".join(line + "\n" for line in out)


class Tally:
    def __init__(self):
        self.total = 0
        self.done = 0
        self.doing = 0

    def add(self, points, status):
        self.total += points
        if status == "done":
            self.done += points
        elif status == "doing":
            self.doing += points

    def merge(self, other):
        self.total += other.total
        self.done += other.done
        self.doing += other.doing

    def pct(self):
        # Floored, so 99.6% never prints as 100%.
        return 100 * self.done // self.total if self.total else 0


def tally(data):
    """Returns (overall, fronts, phases): fronts is a list of
    (front, tally, [(module, tally)]), phases maps phase key -> tally."""
    sizes = data["sizes"]
    post_launch = set(data.get("post_launch_phases", []))
    overall = Tally()
    phases = {key: Tally() for key in data["phases"]}
    fronts = []
    for front in data["fronts"]:
        front_tally = Tally()
        modules = []
        for module in front["modules"]:
            module_tally = Tally()
            for item in module["items"]:
                points = sizes[item["size"]]
                phases[str(item["phase"])].add(points, item["status"])
                # Optional post-launch work (ADR-0025) has its own phase row
                # but must not dilute the launch percentages.
                if str(item["phase"]) not in post_launch:
                    module_tally.add(points, item["status"])
            front_tally.merge(module_tally)
            modules.append((module, module_tally))
        overall.merge(front_tally)
        fronts.append((front, front_tally, modules))
    return overall, fronts, phases


def bar(t, ascii_only, width=BAR_WIDTH):
    if not t.total:
        return " " * width
    done = t.done * width // t.total
    doing = min(width - done, -(-t.doing * width // t.total))
    if ascii_only:
        return "#" * done + "+" * doing + "." * (width - done - doing)
    return "█" * done + "▒" * doing + "░" * (width - done - doing)


def doing_items(data):
    for front in data["fronts"]:
        for module in front["modules"]:
            for item in module["items"]:
                if item["status"] == "doing":
                    yield front, module, item


def render_terminal(data, ascii_only):
    overall, fronts, _ = tally(data)
    name_width = max(len(m["title"]) + 2 for f in data["fronts"] for m in f["modules"])
    name_width = max(name_width, max(len(f["title"]) for f in data["fronts"]))

    def line(name, t):
        extra = "  +%d in progress" % t.doing if t.doing else ""
        return "%-*s %s %3d%%  %3d/%-3d pts%s" % (
            name_width, name, bar(t, ascii_only), t.pct(), t.done, t.total, extra)

    legend = ("# done  + in progress  . todo" if ascii_only
              else "█ done  ▒ in progress  ░ todo")
    out = ["SonoraOS progress (merged and verified only; %s)" % legend, ""]
    for front, front_tally, modules in fronts:
        out.append(line(front["title"], front_tally))
        for module, module_tally in modules:
            out.append(line("  " + module["title"], module_tally))
        out.append("")
    out.append(line("Overall", overall))
    return "\n".join(out) + "\n"


def render_markdown(data):
    overall, fronts, phases = tally(data)

    def row(name, t):
        return "| %s | `%s` | %d%% | %d / %d | %s |" % (
            name, bar(t, False), t.pct(), t.done, t.total,
            "%d pts" % t.doing if t.doing else "")

    head = ["| | Progress | Done | Points | In progress |",
            "|---|---|---:|---:|---:|"]
    out = [
        "# Progress",
        "",
        "<!-- Generated by tools/progress.py --markdown from planning/progress.json."
        " Do not edit by hand; CI regenerates it on main after each merge. -->",
        "",
        "**Overall: %d%%** done (%d of %d points), with %d more points in progress."
        % (overall.pct(), overall.done, overall.total, overall.doing),
        "",
        "`█` done, `▒` in progress, `░` todo.",
        "",
        "## By front",
        "",
    ] + head + [row("**%s**" % f["title"], t) for f, t, _ in fronts] + [
        row("**Overall**", overall), "", "## By phase", "",
    ] + head + [
        row("%s. %s" % (key, data["phases"][key]), phases[key])
        for key in sorted(phases, key=int)
    ] + [""]

    for front, front_tally, modules in fronts:
        out += ["## %s: %d%%" % (front["title"], front_tally.pct()), ""]
        out += head + [row(m["title"], t) for m, t in modules] + [""]

    out += ["## In progress", "", "| Item | Module | Branch | Points |",
            "|---|---|---|---:|"]
    for front, module, item in doing_items(data):
        out.append("| %s | %s / %s | `%s` | %d |" % (
            item["title"], front["title"], module["title"], item["branch"],
            data["sizes"][item["size"]]))
    out += [
        "",
        "## How this is measured",
        "",
        "Every item in `planning/progress.json` has a size (%s) and a status."
        % ", ".join("%s = %d" % kv for kv in sorted(data["sizes"].items(),
                                                     key=lambda kv: kv[1])),
        "A percentage is done points over total points. An item counts as done",
        "only when it is merged to `main` **and** verified, and its `evidence`",
        "names the PR or commit and what verified it. Code that exists but that",
        "no test or run has checked is not done, and neither is anything only",
        "ever run in a container or QEMU when the item is about real hardware.",
        "Work on a branch is shown as in progress and never counted as done.",
        "",
        "A PR that delivers an item flips its status in `progress.json` in the",
        "same PR and leaves this file alone: CI regenerates it on `main` after",
        "the merge.",
        "",
    ]
    return "\n".join(out)


def render_summary(data):
    overall, fronts, _ = tally(data)
    ranked = sorted(fronts, key=lambda f: (-f[1].pct(), f[0]["title"]))
    parts = ", ".join("%s %d%%" % (f["title"], t.pct()) for f, t, _ in ranked[:4])
    return "Progress: %d%% overall (%d/%d pts, +%d in progress); %s\n" % (
        overall.pct(), overall.done, overall.total, overall.doing, parts)


def read_report():
    try:
        with open(REPORT, encoding="utf-8", newline="") as f:
            return f.read()
    except OSError:
        return None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--markdown", action="store_true",
                      help="regenerate planning/PROGRESS.md")
    mode.add_argument("--check", action="store_true",
                      help="exit 1 if the catalogue is invalid")
    mode.add_argument("--check-generated", action="store_true",
                      help="as --check, and exit 1 if PROGRESS.md is stale")
    mode.add_argument("--summary", action="store_true", help="print one line")
    mode.add_argument("--resolve-this-pr", type=int, metavar="N",
                      help='rewrite "this PR" in evidence to "PR #N"')
    mode.add_argument("--pr-from-subject", metavar="SUBJECT",
                      help='print N from a subject ending "(#N)", else nothing')
    parser.add_argument("--ascii", action="store_true",
                        help="ASCII bars, for terminals without Unicode")
    commands = parser.add_subparsers(dest="command")
    get = commands.add_parser("get", help="print one item")
    get.add_argument("id")
    listing = commands.add_parser("list", help="one line per item")
    listing.add_argument("--open", action="store_true",
                         help="only items not done")
    listing.add_argument("--track", metavar="T",
                         help="module id (daw.plugins, plugins) or owner")
    setter = commands.add_parser("set", help="change one item in place")
    setter.add_argument("id")
    setter.add_argument("state", choices=STATUSES)
    setter.add_argument("--evidence", metavar="TEXT")
    setter.add_argument("--pr", type=int, metavar="N",
                        help='evidence becomes "PR #N: TEXT", or "PR #N"')
    setter.add_argument("--branch", metavar="B",
                        help="the branch, which doing needs")
    args = parser.parse_args(argv)
    if args.command and any((args.markdown, args.check, args.check_generated,
                             args.summary, args.resolve_this_pr is not None,
                             args.pr_from_subject is not None)):
        parser.error("%s takes no mode flag" % args.command)

    # Windows consoles default to a legacy code page that cannot encode the
    # bar characters; UTF-8 is what Git Bash's terminal renders.
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")

    if args.pr_from_subject is not None:
        number = pr_from_subject(args.pr_from_subject)
        if number is not None:
            print(number)
        return 0

    try:
        data = load()
    except Invalid as e:
        sys.stderr.write("progress.json is invalid:\n%s\n" % e)
        return 1

    if args.command == "set":
        evidence = args.evidence
        if args.pr is not None:
            evidence = "PR #%d%s" % (args.pr, ": " + evidence if evidence else "")
        with open(CATALOGUE, encoding="utf-8", newline="") as f:
            text = f.read()
        try:
            text = set_item(text, args.id, args.state, evidence, args.branch)
        except Invalid as e:
            sys.stderr.write("progress: %s\n" % e)
            return 1
        with open(CATALOGUE, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        sys.stdout.write(render_item(json.loads(text), args.id))
    elif args.command in ("get", "list"):
        try:
            sys.stdout.write(render_item(data, args.id) if args.command == "get"
                             else render_list(data, args.open, args.track))
        except Invalid as e:
            sys.stderr.write("progress: %s\n" % e)
            return 1
    elif args.resolve_this_pr is not None:
        with open(CATALOGUE, encoding="utf-8", newline="") as f:
            text, count = resolve_this_pr(f.read(), args.resolve_this_pr)
        if count:
            validate(json.loads(text))
            with open(CATALOGUE, "w", encoding="utf-8", newline="") as f:
                f.write(text)
        print('progress: resolved %d "this PR" to "PR #%d"'
              % (count, args.resolve_this_pr))
    elif args.markdown:
        with open(REPORT, "w", encoding="utf-8", newline="\n") as f:
            f.write(render_markdown(data))
        print("wrote %s" % os.path.relpath(REPORT, ROOT).replace(os.sep, "/"))
    elif args.check:
        print("progress: catalogue valid")
    elif args.check_generated:
        if read_report() != render_markdown(data):
            sys.stderr.write("planning/PROGRESS.md is out of date with "
                             "planning/progress.json.\n"
                             "Run: tools/progress.sh --markdown\n")
            return 1
        print("progress: catalogue valid, PROGRESS.md current")
    elif args.summary:
        sys.stdout.write(render_summary(data))
    else:
        sys.stdout.write(render_terminal(data, args.ascii))
    return 0


if __name__ == "__main__":
    sys.exit(main())
