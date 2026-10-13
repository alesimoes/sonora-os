#!/usr/bin/env python3
"""A JACK client that reports the block sizes and rate it was actually given.

Used by tools/test-quantum-entrypoint.sh to show that no client can move the
graph off the pinned quantum and rate (ADR-0003). It talks to whatever
libjack.so.0 the loader finds, which in the runtime container and on the image
is pipewire-jack's, so it exercises the same JACK emulation the engine uses.

It can also be the client that misbehaves:

  --set-buffer-size N   call jack_set_buffer_size(N) once it is active, as a
                        JACK application offering a "buffer size" menu does.
                        In pipewire-jack that sets node.force-quantum on the
                        client, or, with jack.global-buffer-size, writes
                        clock.force-quantum to the settings metadata.
  --set-sample-rate N   call jack_set_sample_rate(N), a pipewire-jack
                        extension that sets node.force-rate on the client.

Other ways of asking (PIPEWIRE_LATENCY, PIPEWIRE_QUANTUM, PIPEWIRE_RATE) are
environment variables that pipewire-jack reads itself, so the caller sets them.

Prints one JSON object on stdout when it finishes. Every block size seen is in
it, so a single 64-frame callback anywhere in the run is visible.
"""

import argparse
import ctypes
import json
import sys
import time

JACK_PORT_IS_INPUT = 0x1
JACK_DEFAULT_AUDIO_TYPE = b"32 bit float mono audio"

ProcessCallback = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_uint32, ctypes.c_void_p)
BufferSizeCallback = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_uint32, ctypes.c_void_p)
SampleRateCallback = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_uint32, ctypes.c_void_p)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="quantum-probe")
    ap.add_argument("--seconds", type=float, default=3.0)
    ap.add_argument("--set-buffer-size", type=int, default=0)
    ap.add_argument("--set-sample-rate", type=int, default=0)
    args = ap.parse_args()

    jack = ctypes.CDLL("libjack.so.0")
    jack.jack_client_open.restype = ctypes.c_void_p
    jack.jack_port_register.restype = ctypes.c_void_p
    jack.jack_port_register.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                        ctypes.c_char_p, ctypes.c_ulong,
                                        ctypes.c_ulong]
    for fn in ("jack_activate", "jack_deactivate", "jack_client_close"):
        getattr(jack, fn).argtypes = [ctypes.c_void_p]
    jack.jack_get_buffer_size.argtypes = [ctypes.c_void_p]
    jack.jack_get_buffer_size.restype = ctypes.c_uint32
    jack.jack_get_sample_rate.argtypes = [ctypes.c_void_p]
    jack.jack_get_sample_rate.restype = ctypes.c_uint32
    jack.jack_set_buffer_size.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    jack.jack_set_sample_rate.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    for fn in ("jack_set_process_callback", "jack_set_buffer_size_callback",
               "jack_set_sample_rate_callback"):
        getattr(jack, fn).argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]

    status = ctypes.c_int(0)
    client = jack.jack_client_open(args.name.encode(), 0, ctypes.byref(status))
    if not client:
        print(json.dumps({"name": args.name, "error": "jack_client_open failed",
                          "status": status.value}))
        return 1

    seen = {"process_calls": 0, "nframes": {}, "bufsize_callbacks": [],
            "rate_callbacks": []}

    # Runs on the JACK thread. Only counts; the report is built afterwards.
    def on_process(nframes, _arg):
        seen["process_calls"] += 1
        seen["nframes"][nframes] = seen["nframes"].get(nframes, 0) + 1
        return 0

    def on_bufsize(nframes, _arg):
        seen["bufsize_callbacks"].append(nframes)
        return 0

    def on_rate(rate, _arg):
        seen["rate_callbacks"].append(rate)
        return 0

    # Kept referenced for the life of the client, or ctypes frees the
    # trampolines while JACK still calls them.
    callbacks = (ProcessCallback(on_process), BufferSizeCallback(on_bufsize),
                 SampleRateCallback(on_rate))
    jack.jack_set_process_callback(client, ctypes.cast(callbacks[0], ctypes.c_void_p), None)
    jack.jack_set_buffer_size_callback(client, ctypes.cast(callbacks[1], ctypes.c_void_p), None)
    jack.jack_set_sample_rate_callback(client, ctypes.cast(callbacks[2], ctypes.c_void_p), None)

    # A port, so the client is a node with something to schedule.
    jack.jack_port_register(client, b"in", JACK_DEFAULT_AUDIO_TYPE, JACK_PORT_IS_INPUT, 0)

    if jack.jack_activate(client) != 0:
        print(json.dumps({"name": args.name, "error": "jack_activate failed"}))
        return 1

    report = {"name": args.name,
              "buffer_size_at_start": jack.jack_get_buffer_size(client),
              "rate_at_start": jack.jack_get_sample_rate(client)}
    if args.set_buffer_size:
        report["set_buffer_size"] = args.set_buffer_size
        report["set_buffer_size_rc"] = jack.jack_set_buffer_size(client, args.set_buffer_size)
    if args.set_sample_rate:
        report["set_sample_rate"] = args.set_sample_rate
        report["set_sample_rate_rc"] = jack.jack_set_sample_rate(client, args.set_sample_rate)

    time.sleep(args.seconds)

    report["buffer_size_at_end"] = jack.jack_get_buffer_size(client)
    report["rate_at_end"] = jack.jack_get_sample_rate(client)
    jack.jack_deactivate(client)
    jack.jack_client_close(client)

    report["process_calls"] = seen["process_calls"]
    report["nframes_seen"] = {str(k): v for k, v in sorted(seen["nframes"].items())}
    report["bufsize_callbacks"] = seen["bufsize_callbacks"]
    report["rate_callbacks"] = seen["rate_callbacks"]
    print(json.dumps(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
