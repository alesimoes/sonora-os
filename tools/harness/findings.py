"""Tester findings as agents actually write them.

The result schema asks for {"severity", "where", "text"} objects, but a
tester sometimes writes a plain string. On 2026-11-07 one such result made
every daemon tick raise (37 tick_errors) and the run was never processed.
"""


def blocking(findings):
    """The findings that send the task back to its engineer.

    A string counts as blocking only when it says so ("blocking: ..."), so
    an informal note never bounces a task; anything else that is not an
    object is ignored."""
    out = []
    for f in findings or []:
        if isinstance(f, dict):
            if f.get("severity") == "blocking":
                out.append(f)
        elif isinstance(f, str) and f.strip().lower().startswith("blocking"):
            out.append({"severity": "blocking", "text": f})
    return out
