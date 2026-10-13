#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs tools/progress.py with whichever Python 3 is installed. On Windows,
# `python3` can be the Microsoft Store stub, which exists on PATH and fails,
# so each candidate is tried rather than just looked up.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

for py in python3 python py; do
    if command -v "$py" >/dev/null 2>&1 &&
        "$py" -c 'import sys; sys.exit(sys.version_info < (3, 7))' >/dev/null 2>&1; then
        exec "$py" "$here/progress.py" "$@"
    fi
done

echo "progress.sh: no Python 3.7 or newer found (tried python3, python, py)" >&2
exit 127
