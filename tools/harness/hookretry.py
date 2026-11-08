"""One bounded retry for hook requests while the daemon restarts.

The broker refuses connections (WinError 10061) for a moment during a restart;
failing closed on that first refusal made agents report blocked. A real answer
from the broker, denial included, is never retried.
"""
import time

RETRY_DELAY_S = 2.0


def with_one_retry(fn, delay=RETRY_DELAY_S, sleep=time.sleep):
    try:
        return fn()
    except OSError:
        sleep(delay)
        return fn()
