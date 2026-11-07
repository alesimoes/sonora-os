"""Which kind of failure a red PR is, and which agent fixes it
(.claude/routing.toml [failures], PROCESS.md "Failure routing").

The classifier reads the CI excerpt the daemon already collects. It is a
heuristic with one safe default: an unrecognised red is a product test red,
fixed by the owner of the changed paths.
"""
import re

# Order matters: the most specific cause first.
_RULES = (
    ("sanitizer_blowup", re.compile(
        r"(sanitizer|asan|tsan)[^\n]*(out of memory|oom|killed|timed? ?out|"
        r"hung|cancell?ed)|(out of memory|oom|killed|cancell?ed)[^\n]*"
        r"(sanitizer|asan|tsan)|internal compiler error|"
        r"c\+\+: fatal error: killed", re.I)),
    ("sanitizer_finding", re.compile(
        r"ThreadSanitizer:|AddressSanitizer:|LeakSanitizer:|"
        r"UndefinedBehaviorSanitizer|RealtimeSanitizer", re.I)),
    ("harness_infra", re.compile(
        r"build-slot|build-lock|micro-build\.sh|docker(?: daemon)?[^\n]*"
        r"(error|cannot|failed)|no space left|image not found|"
        r"pre-pr\.sh|shellcheck|executable bit", re.I)),
    ("flaky_timing_test", re.compile(
        r"timed? ?out|deadline|expected within|took \d+ ?ms|wait_for|"
        r"flak(?:y|e)", re.I)),
)


def classify(note="", jobs=()):
    text = "\n".join([note or "", *jobs])
    for kind, rx in _RULES:
        if rx.search(text):
            return kind
    return "product_test_red"


def agent_for(routing, kind, files=()):
    """The fixer for a failure kind, or None (caller keeps its default)."""
    if routing is None:
        return None
    return routing.agent_for_failure(kind, files)
