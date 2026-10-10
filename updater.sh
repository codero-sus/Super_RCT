#!/bin/sh
# Update this Super_RCT checkout from its git remote.
#   ./updater.sh           check only: fetch and list what would change
#   ./updater.sh --apply   fast-forward to the upstream branch
# The Python interpreter comes from PYTHON= in python.env (see python.env.example).
# Without python.env, python3 or python is used from PATH.
# Wraps tools/update.py. Does not delete itself or any files.
set -e
cd "$(dirname "$0")"
PY=""
if [ -f python.env ]; then
    PY=$(sed -n 's/^[[:space:]]*PYTHON[[:space:]]*=//p' python.env | tail -n 1 \
         | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' -e 's/^"\(.*\)"$/\1/' -e "s/^'\(.*\)'$/\1/")
    if [ -z "$PY" ]; then
        echo "updater.sh: python.env has no PYTHON= value." >&2
        exit 1
    fi
    if [ ! -x "$PY" ] && ! command -v "$PY" >/dev/null 2>&1; then
        echo "updater.sh: Python not found at '$PY' (from python.env)." >&2
        exit 1
    fi
else
    if command -v python3 >/dev/null 2>&1; then
        PY=python3
    elif command -v python >/dev/null 2>&1; then
        PY=python
    else
        echo "updater.sh: no python.env and no python3 on PATH." >&2
        exit 1
    fi
fi
exec "$PY" tools/update.py "$@"
