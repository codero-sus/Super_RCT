#!/bin/sh
# Update this Super_RCT checkout from its git remote.
#   ./updater.sh           check only: fetch and list what would change
#   ./updater.sh --apply   fast-forward to the upstream branch
# Wraps tools/update.py. Does not delete itself or any files.
set -e
cd "$(dirname "$0")"
if command -v python3 >/dev/null 2>&1; then
    exec python3 tools/update.py "$@"
fi
echo "updater.sh: python3 was not found on PATH." >&2
exit 1
