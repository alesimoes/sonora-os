"""A value refreshed by a background thread at a slow interval, for sources
too slow to read on every page refresh (docker df, walking the worktrees)."""
import threading
import time


class Periodic:
    def __init__(self, fn, interval):
        self.fn, self.interval = fn, interval
        self._value, self._at, self._started = None, 0.0, False
        self._lock = threading.Lock()

    def refresh(self):
        """One read now. fn returns a dict, or None when the source is down:
        the previous good value is kept, and before any there is an error."""
        err = "sem dado"
        try:
            v = self.fn()
        except Exception as e:
            v, err = None, str(e)
        with self._lock:
            if v is not None:
                self._value, self._at = v, time.time()
            elif self._value is None:
                self._value = {"ok": False, "error": err}

    def _loop(self):
        while True:
            self.refresh()
            time.sleep(self.interval)

    def value(self):
        """Latest value (None before the first read); starts the thread on first use."""
        with self._lock:
            start, self._started = not self._started, True
        if start:
            threading.Thread(target=self._loop, daemon=True).start()
        with self._lock:
            return self._value

    def age(self):
        with self._lock:
            return time.time() - self._at if self._at else None
