#!/usr/bin/env python3
"""Cheap syntax checks that CI and tools/pre-pr.sh share (2026-11-07: a
printf '%s\n' became two lines in ci.yml, the workflow did not parse and
GitHub ran zero jobs).

  check_syntax.py yaml FILE...            every FILE must parse as YAML
  check_syntax.py quotes --base REF       lines added since REF that end
                                          inside an open quote and are
                                          followed by a line starting with
                                          that quote ("check_syntax: ok" on
                                          the line opts out)

Exit 1 listing every problem as file:line: message.
"""
import re
import subprocess
import sys

CPP_EXT = (".cpp", ".cc", ".h", ".hpp")
QUOTE_EXT = (".yml", ".yaml", ".sh", ".cpp", ".cc", ".h", ".hpp")


def yaml_errors(path):
    import yaml
    try:
        with open(path, encoding="utf-8") as f:
            list(yaml.safe_load_all(f))
    except yaml.YAMLError as e:
        mark = getattr(e, "problem_mark", None)
        line = mark.line + 1 if mark else 0
        return [f"{path}:{line}: invalid YAML: "
                f"{getattr(e, 'problem', None) or e}"]
    return []


def open_quote(line):
    """The quote char a line ends inside, or None. Scans left to right so an
    apostrophe inside a double-quoted string does not count; stops at a
    comment that starts outside a string."""
    q = None
    i = 0
    while i < len(line):
        c = line[i]
        if q:
            if c == "\\" and q == '"':
                i += 1
            elif c == q:
                q = None
        elif c in "'\"":
            q = c
        elif c == "#" and (i == 0 or line[i - 1] in " \t"):
            return None
        i += 1
    return q


def broken_literals(path, lines, wanted):
    """Line numbers (1-based, in wanted) that end inside a quote with the
    next line starting with the same quote."""
    out = []
    if path.endswith(CPP_EXT) and any('R"' in x for x in lines):
        return out   # raw strings span lines legitimately
    for n, line in enumerate(lines[:-1], 1):
        if n not in wanted or "check_syntax: ok" in line:
            continue
        q = open_quote(line.rstrip("\r\n"))
        nxt = lines[n].lstrip()
        if q and nxt.startswith(q):
            out.append(f"{path}:{n}: line ends inside an open {q} string and "
                       f"the next one starts with {q} (did a backslash-n "
                       f"become a real line break?)")
    return out


def added_lines(base):
    """{path: set of added line numbers} from git diff -U0 base...HEAD."""
    diff = subprocess.run(
        ["git", "diff", "-U0", "--diff-filter=ACMR", f"{base}...HEAD"],
        capture_output=True, text=True, encoding="utf-8",
        errors="replace").stdout
    res, path = {}, None
    for line in diff.splitlines():
        if line.startswith("+++ b/"):
            path = line[6:]
            res[path] = set()
        elif line.startswith("@@") and path:
            m = re.search(r"\+(\d+)(?:,(\d+))?", line)
            start, count = int(m.group(1)), int(m.group(2) or 1)
            res[path].update(range(start, start + count))
    return res


def check_quotes(base):
    problems = []
    for path, wanted in added_lines(base).items():
        if not path.endswith(QUOTE_EXT):
            continue
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                lines = f.read().split("\n")
        except OSError:
            continue
        problems += broken_literals(path, lines, wanted)
    return problems


def main(argv):
    if len(argv) >= 2 and argv[0] == "yaml":
        problems = [p for f in argv[1:] for p in yaml_errors(f)]
    elif len(argv) == 3 and argv[0] == "quotes" and argv[1] == "--base":
        problems = check_quotes(argv[2])
    else:
        print(__doc__, file=sys.stderr)
        return 2
    for p in problems:
        print(p, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
