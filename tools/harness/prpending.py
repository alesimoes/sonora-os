"""Branches whose PR the daemon itself is about to open.

The automatic adoption sweep must not take those from under their feature.
Everything else is an orphan and is adopted: a sub-branch (`sub/...`) is
never opened by the daemon, and a task or feature that is finished,
blocked or closed will not open anything either. #519 was opened from a
sub-branch and, with the old rule ("any branch of a task with no pr"), was
skipped by every sweep.
"""

PRE_PR_FEATURE = ("planning", "distributing", "queued", "designing",
                  "building", "bugfix", "qa", "integrating")
SUB_PREFIX = "sub/"


def _live(x):
    return (x.get("state") in PRE_PR_FEATURE) and not x.get("pr")


def in_the_making(store):
    """Set of branch names the daemon will push and open a PR for."""
    out = set()
    for x in store.features() + store.tasks():
        b = x.get("branch")
        if b and not b.startswith(SUB_PREFIX) and _live(x):
            out.add(b)
    return out
