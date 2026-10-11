# 2026-10-11 — a comment inside a line continuation silently dropped arguments

## Symptom

`tools/test-boot-qemu.sh` ran for over ten minutes with no output at all and
had to be killed. A QEMU process was running and consuming memory. No serial
log was created, and no `qemu.log` either, despite the invocation redirecting
to both.

## Root cause

While addressing review findings I added explanatory comments between the
arguments of the `qemu-system-x86_64` invocation, which is one command spread
over fourteen backslash-continued lines:

    "$QEMU" \
      -kernel ... \
      # Mirrors the shipped --bootappend-live ...
      -append "... sonora.smoketest" \
      ...

A backslash continuation joins the next line onto the current logical line,
and a `#` in the joined result comments out everything from there to the end
of that logical line. So `-append`, `-serial`, `-display none`,
`-monitor none`, `-no-reboot` and both redirections were discarded.

QEMU therefore started with no smoke test parameter, no serial output and no
`-display none`, which is why it opened a window and why the harness waited
its full timeout for output that could never arrive.

`bash -n` does not catch this. The result is syntactically valid; it simply
means something else.

## Fix

The comments are hoisted above the command, where a human still reads them and
the shell cannot eat them. Checking the other ten shell files in the
repository found no further instance.

## Guard against recurrence

The detector is one line of awk: flag any comment whose previous line ends in
a backslash. Worth running over `tools/*.sh` whenever a long invocation is
edited.

## The other half of this incident

Separately, and worse: I reported a fix to `sonora-smoketest` as applied when
the file on disk did not contain it. The edit printed its success message and
the following syntax check passed, so nothing contradicted the claim. It
surfaced only because the rebuilt ISO still ran the old script — and the md5
of the file inside the image matched the md5 in the repository, which proved
the image was faithful and the repository was wrong.

Same lesson as the masked build exit code: a tool reporting success is not
evidence that the intended change exists. Check the artefact. Every edit to a
file that ends up inside the image is now followed by a `grep` for the thing
that was supposed to change, and the ISO is inspected rather than trusted.
