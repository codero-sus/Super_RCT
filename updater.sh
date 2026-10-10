#!/usr/bin/env bash
# Update this Super_RCT checkout from its git remote. Works on Linux and macOS.
# Runs under bash: dash and other sh shells drop the environment variable 2PY2 at startup.
#   ./updater.sh           check only: fetch and list what would change
#   ./updater.sh --apply   fast-forward to the upstream branch
#
# Python interpreter, first match wins:
#   1. PYTHON= in python.env in this folder (optional file)
#   2. environment variable 2PY2 (the "second Python": portable or embeddable builds)
#   3. python3, then python, from PATH
# Wraps tools/update.py. Does not delete itself or any files.
cd "$(dirname "$0")" || exit 1

PY=""
PYSRC=""

if [ -f python.env ]; then
    v=$(sed -n 's/^[[:space:]]*PYTHON[[:space:]]*=//p' python.env | tail -n 1 \
        | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' \
              -e 's/^"\(.*\)"$/\1/' -e "s/^'\(.*\)'$/\1/")
    if [ -n "$v" ]; then PY=$v; PYSRC="python.env"; fi
fi

if [ -z "$PY" ]; then
    v=$(printenv 2PY2 2>/dev/null || true)
    if [ -n "$v" ]; then PY=$v; PYSRC="environment variable 2PY2"; fi
fi

if [ -z "$PY" ]; then
    for c in python3 python; do
        if command -v "$c" >/dev/null 2>&1; then PY=$c; PYSRC="PATH"; break; fi
    done
fi

if [ -z "$PY" ]; then
    echo "updater.sh: no Python found. Set PYTHON= in python.env, set 2PY2, or install python3." >&2
    exit 1
fi
if [ ! -x "$PY" ] && ! command -v "$PY" >/dev/null 2>&1; then
    echo "updater.sh: Python not found at '$PY' (from $PYSRC)." >&2
    exit 1
fi

exec "$PY" tools/update.py "$@"
