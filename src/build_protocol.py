"""Which chat protocol this build serves.

The builder writes this file for each release executable: "telegram" or
"discord" gives a build that runs only that protocol. "both" is the source
default, used by the tests and by people running the script directly.
"""

PROTOCOL = "both"
