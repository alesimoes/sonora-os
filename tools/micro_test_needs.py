"""Targets a matched ctest test needs besides the one that matched it.

A test is matched through one artifact in its command, but the command can
name more: a driver executable, or the object files of an OBJECT library
(the rt_reachability canaries). Those are not dependents of the changed
code, so the plan has to add them or the test runs against a missing file
(micro build on PR #470: "Could not find executable", "nm: No such file").
"""
import os

KINDS = ("EXECUTABLE", "OBJECT_LIBRARY", "STATIC_LIBRARY", "SHARED_LIBRARY",
         "MODULE_LIBRARY")


def _words(command):
    out = set()
    for w in command or []:
        for x in [str(w)] + str(w).replace("=", " ").replace(";", " ").split():
            out.add(x.replace("\\", "/"))
    return out


def _needs(target, words, basenames):
    if target["type"] == "OBJECT_LIBRARY":
        mark = "/CMakeFiles/%s.dir/" % target["name"]
        return any(mark in w for w in words)
    return any(os.path.basename(a.replace("\\", "/")) in basenames
               for a in target["artifacts"])


def needed_targets(ctest_json, targets, test_names):
    """Names of buildable targets that the commands of `test_names` name."""
    chosen = set(test_names)
    words = set()
    for test in ctest_json.get("tests", []):
        if test["name"] in chosen:
            words |= _words(test.get("command"))
    basenames = {os.path.basename(w) for w in words}
    return sorted(t["name"] for t in targets
                  if t["type"] in KINDS and _needs(t, words, basenames))
