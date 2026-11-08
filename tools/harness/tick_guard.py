"""One bad item never stops the tick (2026-11-08 audit).

A tester finding written as a string raised inside _on_result; the tick is
a straight sequence, so for an hour every tick died there and the daemon
skipped PR watching, merges, status and self-update (#504 fixed the string,
not the shape). The same is true of any step: a gh outage in one PR's poll
must not stop the next PR's merge.

Guard.step runs one step (or one item of a loop) and swallows its exception:
the first occurrence of an error text is an event and a stderr traceback,
repeats only count, and the error stands on the dashboard (status.json
`alerts`) until the same step succeeds again.
"""
import traceback


class Guard:
    def __init__(self, store, clock, log=None):
        self.store, self.clock = store, clock
        self.log = log or (lambda m: None)
        self.errors = {}   # (step, key) -> {text, count, first, last, task}

    def step(self, name, fn, *args, key=None, task=None, **kw):
        """fn(*args, **kw); its value, or None when it raised."""
        k = (name, key)
        try:
            out = fn(*args, **kw)
        except Exception as e:  # noqa: BLE001 - the whole point
            self._failed(k, e, task)
            return None
        self.errors.pop(k, None)
        return out

    def each(self, name, items, fn, *args, key=lambda x: x, task=None):
        """fn(item, *args) per item, each isolated. A step that returns
        False stops the loop (a rate limit: polling more is pointless)."""
        for it in items:
            out = self.step(name, fn, it, *args, key=key(it),
                            task=task(it) if task else None)
            if out is False:
                return False
        return True

    def _failed(self, k, e, task):
        text = f"{type(e).__name__}: {e}"[:300]
        cur = self.errors.get(k)
        now = self.clock()
        if cur is None or cur["text"] != text:
            where = k[0] + (f"[{k[1]}]" if k[1] is not None else "")
            self.store.event(task, "step_error", f"{where}: {text}")
            self.log(f"step {where} failed: {text}\n"
                     + traceback.format_exc())
            self.errors[k] = {"text": text, "count": 1, "first": now,
                              "last": now, "task": task}
        else:
            cur["count"] += 1
            cur["last"] = now

    def rows(self):
        """Dashboard alert rows (same shape as supervisor.Alert.row)."""
        out = []
        for (name, key), e in sorted(self.errors.items(),
                                     key=lambda kv: kv[1]["first"]):
            where = name + (f" {key}" if key is not None else "")
            out.append({"id": f"tick:{name}:{key}", "level": "alert",
                        "text": f"tick step {where} fails: {e['text']} "
                                f"(x{e['count']}); the rest of the tick "
                                "goes on", "since": e["first"],
                        "last": e["last"], "count": e["count"],
                        "check": "tick"})
        return out
