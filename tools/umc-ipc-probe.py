#!/usr/bin/env python3
"""SOCK_SEQPACKET request/reply round trip between two processes (ADR-0007
open question 1). Meant to run while the engine records, so the figure is the
one under RT load. Prints one JSON object: count, p50/p99/max in microseconds.

The request is 64 bytes, paced at --hz, the shape of a UI command. An echo
child stands in for the engine's control socket; the cost measured is the
kernel's wake-up and scheduling, not the engine's command handling.
"""
import argparse
import json
import os
import socket
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--hz", type=float, default=200.0)
    args = ap.parse_args()

    a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    pid = os.fork()
    if pid == 0:
        a.close()
        try:
            while True:
                data = b.recv(64)
                if not data:
                    break
                b.send(data)
        finally:
            os._exit(0)
    b.close()
    msg = b"x" * 64
    period = 1.0 / args.hz
    end = time.monotonic() + args.seconds
    samples = []
    nxt = time.monotonic()
    while time.monotonic() < end:
        t0 = time.perf_counter_ns()
        a.send(msg)
        a.recv(64)
        samples.append((time.perf_counter_ns() - t0) / 1000.0)
        nxt += period
        d = nxt - time.monotonic()
        if d > 0:
            time.sleep(d)
    a.close()
    os.waitpid(pid, 0)
    samples.sort()
    n = len(samples)

    def pct(p):
        return samples[min(n - 1, int(p * n))]

    print(json.dumps({"count": n, "p50_us": round(pct(0.50), 1),
                      "p99_us": round(pct(0.99), 1),
                      "max_us": round(samples[-1], 1)}))


if __name__ == "__main__":
    main()
