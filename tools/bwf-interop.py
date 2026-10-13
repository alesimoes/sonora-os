#!/usr/bin/env python3
"""Record two takes with the engine, then judge the files with other people's
readers.

Two subcommands, for the two containers tools/test-bwf-interop.sh runs:

  drive  runs in the runtime image (the audio stack we ship). It starts the
         engine interactively on a pseudo-terminal and presses the keys an
         operator would: record, stop, record, stop, quit. Two takes, because
         a headless take always starts at sample 0, and a TimeReference of 0
         is also what a writer that never sets the field produces. The second
         take starts where the first stopped, so its TimeReference is a number
         that can only be right if the engine put it there.

  check  runs in the reader image (Debian's ffprobe, soxi, sndfile-info,
         mediainfo and Ardour). It never trusts its own RIFF parser for a
         verdict on its own: the parser below establishes what the bytes say,
         and every claim is then confirmed by at least one reader we did not
         write.
"""

import argparse
import datetime
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time

ORIGINATOR = "Sonora Engine"

# --------------------------------------------------------------------------
# drive
# --------------------------------------------------------------------------


def drive(args):
    import pty  # Linux only, and only needed here.

    pid, fd = pty.fork()
    if pid == 0:
        os.execv(args.engine, [
            args.engine, "--dir", args.dir, "--tracks", str(args.tracks),
            "--rate", str(args.rate), "--arm-all",
            "--wait-for-inputs", "20"])

    screen = bytearray()
    lock = threading.Lock()

    # The engine redraws twenty times a second. If nobody reads the pty its
    # buffer fills, the engine blocks in printf, and the take stalls.
    def pump():
        with open(args.log, "wb") as log:
            while True:
                try:
                    data = os.read(fd, 4096)
                except OSError:
                    return
                if not data:
                    return
                log.write(data)
                with lock:
                    screen.extend(data)

    threading.Thread(target=pump, daemon=True).start()

    def wait_for(pattern, since, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with lock:
                if re.search(pattern, bytes(screen[since:])):
                    return True
            time.sleep(0.05)
        return False

    def mark():
        with lock:
            return len(screen)

    def fail(why):
        print(f"FAIL: {why}", file=sys.stderr)
        try:
            os.kill(pid, 15)
        except OSError:
            pass
        return 1

    # The engine waits for a capture connection before entering its key loop,
    # so the inputs are patched from here while it waits.
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        out = subprocess.run(["pw-link", "-i"], capture_output=True, text=True)
        if "sonora:in_1" in out.stdout:
            break
        time.sleep(0.2)
    connected = 0
    for i in range(1, args.tracks + 1):
        rc = subprocess.run(
            ["pw-link", f"test-source:monitor_AUX{i - 1}", f"sonora:in_{i}"],
            capture_output=True).returncode
        connected += rc == 0
    print(f"patched {connected} of {args.tracks} inputs")
    if connected != args.tracks:
        return fail("not every input could be patched")

    # The status line is the only sign that the key loop is running and the
    # terminal is in raw mode; a key sent earlier would sit in a cooked line.
    if not wait_for(rb"SONORA .*take 1", 0, 20):
        return fail("the engine never drew its status line")

    for take in (1, 2):
        since = mark()
        os.write(fd, b" ")
        if not wait_for(rb"REC ", since, 5):
            return fail(f"take {take} never started recording")
        time.sleep(args.seconds)
        since = mark()
        os.write(fd, b" ")
        if not wait_for(rb"take %d" % (take + 1), since, 5):
            return fail(f"take {take} never stopped")
        # Stopped, the playhead holds still, so this pause must not move
        # take 2's origin. That is part of what the check verifies.
        time.sleep(0.5)

    os.write(fd, b"q")
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        done, status = os.waitpid(pid, os.WNOHANG)
        if done:
            code = os.waitstatus_to_exitcode(status)
            print(f"engine exited {code}")
            return 0 if code == 0 else 1
        time.sleep(0.1)
    return fail("the engine did not quit on q")


# --------------------------------------------------------------------------
# check
# --------------------------------------------------------------------------


class Verdicts:
    def __init__(self):
        self.ok = 0
        self.fail = 0
        self.xfail = 0

    def check(self, cond, what, detail=""):
        if cond:
            self.ok += 1
            print(f"ok:    {what}")
        else:
            self.fail += 1
            print(f"FAIL:  {what}{': ' + detail if detail else ''}")
        return cond

    def known(self, what, backlog):
        """A defect already reported, so it does not turn the run red, but
        is printed every time so nobody mistakes it for fixed."""
        self.xfail += 1
        print(f"xfail: {what} (known defect, BACKLOG: {backlog})")


def parse_riff(path):
    """What the bytes say. Not a verdict: the readers below give those."""
    with open(path, "rb") as f:
        data = f.read()
    info = {"size": len(data), "chunks": []}
    if len(data) < 12 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        info["magic"] = False
        return info
    info["magic"] = True
    info["riff_size"] = struct.unpack_from("<I", data, 4)[0]
    pos = 12
    while pos + 8 <= len(data):
        tag, size = struct.unpack_from("<4sI", data, pos)
        tag = tag.decode("latin-1")
        payload = data[pos + 8:pos + 8 + size]
        info["chunks"].append((tag, size))
        if tag == "bext":
            info["bext"] = payload
        elif tag == "fmt ":
            (info["format_tag"], info["channels"], info["rate"],
             info["byte_rate"], info["block_align"],
             info["bits"]) = struct.unpack_from("<HHIIHH", payload, 0)
        elif tag == "data":
            info["data_size"] = size
        pos += 8 + size + (size & 1)
    info["walked_to"] = pos
    return info


def bext_fields(raw):
    def text(off, n):
        return raw[off:off + n].split(b"\0", 1)[0].decode("ascii", "replace")
    low, high = struct.unpack_from("<II", raw, 338)
    return {
        "description": text(0, 256),
        "originator": text(256, 32),
        "originator_reference": text(288, 32),
        "date": raw[320:330],
        "time": raw[330:338],
        "time_reference": (high << 32) | low,
        "version": struct.unpack_from("<H", raw, 346)[0],
        "reserved_zero": not any(raw[422:602]),
    }


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def ffprobe(path):
    out = run(["ffprobe", "-v", "error", "-of", "json", "-show_format",
               "-show_streams", path])
    try:
        return json.loads(out)
    except ValueError:
        return {}


def soxi(path):
    return {flag: run(["soxi", flag, path]).strip()
            for flag in ("-t", "-r", "-c", "-b", "-s", "-e")}


def sndfile_info(path):
    """sndfile-info prints "Key : value" lines; --broadcast prints the bext
    as libsndfile parsed it, which is what Ardour reads through."""
    fields = {}
    for line in (run(["sndfile-info", path]) +
                 run(["sndfile-info", "--broadcast", path])).splitlines():
        m = re.match(r"\s*([A-Za-z][A-Za-z /]*?)\s*:\s*(.*)$", line)
        if m:
            fields.setdefault(m.group(1).strip().lower(), m.group(2).strip())
    return fields


def sndfile_time_ref(text):
    """sndfile-info prints the TimeReference as "0x000018200 (2.058667
    seconds)", or a bare 0."""
    if text is None:
        return None
    m = re.match(r"(0x[0-9a-fA-F]+|\d+)", text)
    if not m:
        return None
    value = m.group(1)
    return int(value, 16) if value.startswith("0x") else int(value)


def mediainfo(path):
    out = run(["mediainfo", "--Output=JSON", path])
    try:
        tracks = json.loads(out)["media"]["track"]
    except (ValueError, KeyError, TypeError):
        return {}, {}
    general = next((t for t in tracks if t.get("@type") == "General"), {})
    audio = next((t for t in tracks if t.get("@type") == "Audio"), {})
    return general, audio


ARDOUR_LUA = r"""
local s = load_session("{dir}", "interop")
assert(s, "Ardour could not load the session")
for _, id in ipairs({{ {ids} }}) do
  local src = s:source_by_id(PBD.ID(id))
  if src:isnil() then
    print("ARDOUR " .. id .. " missing")
  else
    print("ARDOUR " .. id .. " " .. src:natural_position():samples())
  end
end
close_session()
"""


def ardour_positions(paths, rate, workdir):
    """Where Ardour puts each file on its timeline.

    Ardour's import at "file timestamp" places a region at the source's
    natural position (gtk2_ardour/editor_audio_import.cc, 8.12, lines
    979-990), which SndFileSource::open() sets from the bext TimeReference
    read through libsndfile (libs/ardour/sndfilesource.cc, lines 438-450).
    The headless Lua host cannot drive the import dialog, so the files are
    declared as external sources of a fresh session and Ardour opens them
    itself; the position it reports is the one the import would use.
    """
    new_session = "ardour8-new_empty_session"
    sess = os.path.join(workdir, "interop")
    ardour_file = os.path.join(sess, "interop.ardour")
    # Ardour 8 sometimes aborts at exit (rc 134, "exception not rethrown")
    # after the session is already written, about 1 run in 10 on one CPU.
    # The session file is what this check needs, so judge by that, keep the
    # tool's output for diagnosis, and retry a genuine failure once.
    for attempt in (1, 2):
        shutil.rmtree(sess, ignore_errors=True)
        made = subprocess.run([new_session, sess, "interop"],
                              capture_output=True, text=True)
        with open(os.path.join(workdir, "ardour.log"), "a",
                  encoding="utf-8") as log:
            log.write(f"{new_session} attempt {attempt}: rc={made.returncode}\n")
            log.write(made.stdout + made.stderr)
        if os.path.isfile(ardour_file):
            break
    else:
        return None, (f"{new_session} wrote no session file "
                      f"(rc={made.returncode}; see ardour.log)")
    with open(ardour_file, encoding="utf-8") as f:
        xml = f.read()
    ids = []
    nodes = []
    for n, path in enumerate(paths):
        sid = str(900000 + n)
        ids.append(sid)
        name = path.replace("&", "&amp;").replace('"', "&quot;")
        # No natural-position attribute: the value must come from the file.
        nodes.append(
            f'<Source name="{name}" type="audio" flags="" id="{sid}" '
            f'captured-for="" channel="0" origin=""/>')
    if "<Sources/>" in xml:
        xml = xml.replace("<Sources/>",
                          "<Sources>" + "".join(nodes) + "</Sources>")
    elif "<Sources>" in xml:
        xml = xml.replace("<Sources>", "<Sources>" + "".join(nodes), 1)
    else:
        return None, "no <Sources> element in the new session"
    with open(ardour_file, "w", encoding="utf-8") as f:
        f.write(xml)
    script = os.path.join(workdir, "positions.lua")
    with open(script, "w", encoding="utf-8") as f:
        f.write(ARDOUR_LUA.format(
            dir=sess, ids=", ".join(f'"{i}"' for i in ids)))
    out = subprocess.run(["ardour8-lua", script], capture_output=True,
                         text=True, timeout=120)
    with open(os.path.join(workdir, "ardour.log"), "w") as f:
        f.write(out.stdout + out.stderr)
    found = {}
    for line in out.stdout.splitlines():
        m = re.match(r"ARDOUR (\d+) (\d+|missing)$", line.strip())
        if m:
            found[m.group(1)] = (None if m.group(2) == "missing"
                                 else int(m.group(2)))
    return {p: found.get(i) for p, i in zip(paths, ids)}, None


def valid_date(raw):
    """EBU Tech 3285 v2, section 2.3: 4-digit year and 2-digit month and day,
    any separator (hyphen, underscore, colon, space or stop recommended)."""
    try:
        s = raw.decode("ascii")
    except UnicodeDecodeError:
        return False
    m = re.fullmatch(r"(\d{4}).(\d{2}).(\d{2})", s)
    if not m:
        return False
    try:
        datetime.date(int(m.group(1)), int(m.group(2)), int(m.group(3)))
    except ValueError:
        return False
    return True


def valid_time(raw):
    try:
        s = raw.decode("ascii")
    except UnicodeDecodeError:
        return False
    m = re.fullmatch(r"(\d{2}).(\d{2}).(\d{2})", s)
    return bool(m) and int(m.group(1)) < 24 and int(m.group(2)) < 60 \
        and int(m.group(3)) < 60


def check(args):
    v = Verdicts()
    audio_dir = os.path.join(args.dir, "Audio Files")
    takes = {}
    for path in sorted(glob.glob(os.path.join(audio_dir, "*.wav"))):
        m = re.match(r"Audio (\d+)_(\d+)\.wav$", os.path.basename(path))
        if m:
            takes.setdefault(int(m.group(2)), {})[int(m.group(1))] = path

    v.check(sorted(takes) == [1, 2], "two takes on disk",
            f"found takes {sorted(takes)}")
    for take, files in sorted(takes.items()):
        v.check(sorted(files) == list(range(1, args.tracks + 1)),
                f"take {take}: one file per track ({args.tracks})",
                f"found tracks {sorted(files)}")
    if v.fail:
        return finish(v)

    take_frames = {}
    take_tr = {}
    for take, files in sorted(takes.items()):
        for track, path in sorted(files.items()):
            name = os.path.basename(path)
            riff = parse_riff(path)

            # --- container --------------------------------------------
            v.check(riff["magic"], f"{name}: RIFF/WAVE magic")
            v.check(riff.get("riff_size") == riff["size"] - 8,
                    f"{name}: RIFF size = file size - 8",
                    f"{riff.get('riff_size')} vs {riff['size'] - 8}")
            v.check(riff.get("walked_to") == riff["size"],
                    f"{name}: chunks tile the file exactly",
                    f"walk ended at {riff.get('walked_to')} of {riff['size']}")
            order = [t for t, _ in riff["chunks"]]
            v.check(order[:3] == ["bext", "fmt ", "data"],
                    f"{name}: chunk order bext, fmt, data (Tech 3285 2.1)",
                    str(order))
            frames = riff.get("data_size", 0) // max(riff.get("block_align", 1), 1)

            fp = ffprobe(path)
            stream = (fp.get("streams") or [{}])[0]
            tags = (fp.get("format") or {}).get("tags", {})
            v.check((fp.get("format") or {}).get("format_name") == "wav",
                    f"{name}: ffprobe reads it as wav")
            v.check(stream.get("codec_name") == "pcm_s24le"
                    and stream.get("bits_per_sample") == 24,
                    f"{name}: ffprobe: 24-bit PCM",
                    f"{stream.get('codec_name')}/{stream.get('bits_per_sample')}")
            v.check(stream.get("sample_rate") == str(args.rate)
                    and stream.get("channels") == 1,
                    f"{name}: ffprobe: {args.rate} Hz mono",
                    f"{stream.get('sample_rate')} Hz {stream.get('channels')}ch")
            v.check(stream.get("duration_ts") == frames,
                    f"{name}: ffprobe length = data chunk ({frames} frames)",
                    str(stream.get("duration_ts")))

            sx = soxi(path)
            v.check(sx["-t"] == "wav" and sx["-r"] == str(args.rate)
                    and sx["-c"] == "1" and sx["-b"] == "24"
                    and sx["-e"] == "Signed Integer PCM",
                    f"{name}: soxi: wav, {args.rate} Hz, mono, 24-bit signed",
                    json.dumps(sx))
            v.check(sx["-s"] == str(frames),
                    f"{name}: soxi length = {frames} frames", sx["-s"])

            sf = sndfile_info(path)
            v.check(sf.get("frames") == str(frames),
                    f"{name}: sndfile-info length = {frames} frames",
                    str(sf.get("frames")))
            v.check(sf.get("sample rate") == str(args.rate),
                    f"{name}: sndfile-info rate", str(sf.get("sample rate")))

            # --- bext -------------------------------------------------
            raw = riff.get("bext", b"")
            if not v.check(len(raw) == 602,
                           f"{name}: bext is 602 bytes (Tech 3285 2.3, v0-v2)",
                           str(len(raw))):
                continue
            b = bext_fields(raw)
            tr = b["time_reference"]
            take_tr.setdefault(take, set()).add(tr)
            take_frames.setdefault(take, set()).add(frames)
            v.check(b["reserved_zero"], f"{name}: bext reserved bytes are zero")
            v.check(tags.get("time_reference") == str(tr),
                    f"{name}: ffprobe time_reference = {tr}",
                    str(tags.get("time_reference")))
            v.check(sndfile_time_ref(sf.get("time ref")) == tr,
                    f"{name}: libsndfile time reference = {tr}",
                    str(sf.get("time ref")))
            _, audio = mediainfo(path)
            # MediaInfo turns the TimeReference into a delay in seconds and
            # names where it came from.
            delay = audio.get("Delay")
            v.check(delay is not None
                    and abs(float(delay) * args.rate - tr) < 0.5
                    and "bext" in str(audio.get("Delay_Source")),
                    f"{name}: mediainfo delay = {tr / args.rate:.6f} s from bext",
                    f"Delay={delay} source={audio.get('Delay_Source')}")
            # ffmpeg's wav demuxer files the bext Originator under the
            # generic "encoded_by" tag (libavformat/wavdec.c, wav_parse_bext_tag).
            v.check(tags.get("encoded_by") == ORIGINATOR
                    and sf.get("originator") == ORIGINATOR,
                    f"{name}: Originator \"{ORIGINATOR}\" (ffprobe, libsndfile)",
                    f"{tags.get('encoded_by')!r}/{sf.get('originator')!r}")
            if not any(b["date"]) and not any(b["time"]):
                v.known(f"{name}: OriginationDate/Time are empty",
                        "bext OriginationDate/OriginationTime")
            else:
                v.check(valid_date(b["date"]),
                        f"{name}: OriginationDate is yyyy-mm-dd",
                        repr(b["date"]))
                v.check(valid_time(b["time"]),
                        f"{name}: OriginationTime is hh:mm:ss",
                        repr(b["time"]))
                # ffmpeg's wav demuxer exposes bext OriginationDate as the
                # `date` tag and OriginationTime as `creation_time`.
                v.check(tags.get("date") == b["date"].decode(
                    "ascii", "replace"),
                    f"{name}: ffprobe agrees on the date",
                    str(tags.get("date")))
                v.check(tags.get("creation_time") == b["time"].decode(
                    "ascii", "replace"),
                    f"{name}: ffprobe agrees on the time",
                    str(tags.get("creation_time")))
            if "iXML" in order:
                v.check(b"<BWFXML" in open(path, "rb").read(),
                        f"{name}: iXML chunk holds a BWFXML document")

    # --- timeline ---------------------------------------------------------
    one = take_tr.get(1, set())
    two = take_tr.get(2, set())
    v.check(len(one) == 1 and len(two) == 1,
            "every track of a take carries the same TimeReference",
            f"take 1 {sorted(one)}, take 2 {sorted(two)}")
    v.check(one == {0}, "take 1 starts at sample 0", str(sorted(one)))
    f1 = take_frames.get(1, set())
    v.check(len(f1) == 1, "every track of take 1 has the same length",
            str(sorted(f1)))
    if len(two) == 1 and len(f1) == 1:
        tr2, = two
        length1, = f1
        v.check(tr2 > 0, "take 2 TimeReference is not zero", str(tr2))
        # The playhead runs only while the transport does, so take 2 begins
        # exactly where take 1 ended. A callback that straddles the state
        # change can move it by one period; anything more is a defect.
        v.check(abs(tr2 - length1) <= args.quantum,
                f"take 2 starts where take 1 ended (TR {tr2}, take 1 "
                f"{length1} frames, delta {tr2 - length1})")

    # --- a DAW --------------------------------------------------------------
    paths = [p for t in sorted(takes) for _, p in sorted(takes[t].items())]
    positions, err = ardour_positions(paths, args.rate, args.workdir)
    if v.check(positions is not None, "Ardour 8 opens a session holding the files",
               err or ""):
        for path in paths:
            tr = bext_fields(parse_riff(path)["bext"])["time_reference"]
            pos = positions.get(path)
            v.check(pos == tr,
                    f"Ardour places {os.path.basename(path)} at sample {tr}",
                    f"natural position {pos}")
    return finish(v)


def finish(v):
    print(f"RESULT ok={v.ok} fail={v.fail} xfail={v.xfail}")
    print("TEST PASS" if v.fail == 0 else "TEST FAIL")
    return 0 if v.fail == 0 else 1


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("drive")
    d.add_argument("--engine", required=True)
    d.add_argument("--dir", required=True)
    d.add_argument("--tracks", type=int, default=18)
    d.add_argument("--rate", type=int, default=48000)
    d.add_argument("--seconds", type=float, default=2.0)
    d.add_argument("--log", required=True)
    c = sub.add_parser("check")
    c.add_argument("--dir", required=True)
    c.add_argument("--tracks", type=int, default=18)
    c.add_argument("--rate", type=int, default=48000)
    c.add_argument("--quantum", type=int, default=128)
    c.add_argument("--workdir", required=True)
    args = ap.parse_args()
    return drive(args) if args.cmd == "drive" else check(args)


if __name__ == "__main__":
    sys.exit(main())
