"""Which of a feature's work tasks still count for it.

A slice ends "closed" when a re-plan replaces it, triage finds it already on
main, or the owner cancels it. It will never reach a done state, so a closed
slice must not stand for its feature (the integrator and fixer launch on the
first live task) nor hold its integration, its PR or its state. 2026-11-13:
ui.record.punch_loop's closed .ui kept .ui-r1 integrating with no agent.
"""

RETIRED = ("closed",)


def live(tasks):
    """The tasks that still count, in the order given."""
    return [t for t in tasks if t["state"] not in RETIRED]


def by_feature(tasks):
    """{feature id (or the task's own id): live work tasks}, in task order."""
    out = {}
    for t in live(tasks):
        if t["kind"] == "work":
            out.setdefault(t["feature_id"] or t["id"], []).append(t)
    return out


def holding(tasks, done_states):
    """The live slices that keep a feature's finished ones from integrating
    (its integrator waits for every slice), or [] when no slice waits.
    2026-11-13: three features showed finished slices "integrating" for
    hours and nothing said which blocked sibling held them."""
    mine = live(tasks)
    if not any(t["state"] == "integrating" for t in mine):
        return []
    return [t for t in mine if t["state"] not in done_states]
