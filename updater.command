#!/usr/bin/env bash
# macOS: double-click this file in Finder to check or update. Terminal opens and shows the result.
# Same as ./updater.sh; the window waits for Return so you can read the output.
cd "$(dirname "$0")" || exit 1
./updater.sh "$@"
rc=$?
echo
printf 'Press Return to close... '
read -r _
exit $rc
