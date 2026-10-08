#!/usr/bin/env python3
"""Builds the Linux installers into build/installers/ (not committed).

  python3 tools/build_installers.py

Python installer: src/install/linux.py with the bot bundle embedded (a zip of
the Python files, base64). C installer: src/install/linux.c with the release
bot binary embedded (base64), compiled with cc. Each installer then extracts
its embedded bot into the directory it is run from.
"""

import base64
import io
import os
import re
import stat
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build", "installers")
PY_BUNDLE = ["super_rct.py", "monitor.py", "discord_transport.py", "commands_table.py"]


def python_installer():
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for name in PY_BUNDLE:
            z.write(os.path.join(ROOT, "src", name), name)
    payload = base64.b64encode(buf.getvalue()).decode()

    with open(os.path.join(ROOT, "src", "install", "linux.py"), encoding="utf-8") as f:
        src = f.read()
    src, count = re.subn(r'^PAYLOAD = ""', f'PAYLOAD = "{payload}"', src, count=1, flags=re.M)
    assert count == 1, "payload placeholder not found in linux.py"

    dest = os.path.join(OUT, "super_rct-linux.py")
    with open(dest, "w", encoding="utf-8") as f:
        f.write(src)
    os.chmod(dest, os.stat(dest).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return dest


def c_installer():
    binary = os.path.join(ROOT, "build", "super_rct")
    subprocess.run(["make", "-C", ROOT, "release"], check=True, stdout=subprocess.DEVNULL)
    with open(binary, "rb") as f:
        payload = base64.b64encode(f.read()).decode()

    with open(os.path.join(ROOT, "src", "install", "linux.c"), encoding="utf-8") as f:
        src = f.read()
    chunks = [payload[i:i + 4000] for i in range(0, len(payload), 4000)] or [""]
    literal = ",\n".join(f'    "{c}"' for c in chunks)
    src, count = re.subn(r'^static const char \*const PAYLOAD_PARTS\[\] = \{""\};',
                         "static const char *const PAYLOAD_PARTS[] = {\n" + literal + "};",
                         src, count=1, flags=re.M)
    assert count == 1, "payload placeholder not found in linux.c"

    src_path = os.path.join(OUT, "super_rct-linux.c")
    with open(src_path, "w", encoding="utf-8") as f:
        f.write(src)
    dest = os.path.join(OUT, "super_rct-linux")
    subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Wshadow",
                    "-Wformat=2", "-s", "-o", dest, src_path], check=True)
    os.remove(src_path)
    return dest


def main():
    os.makedirs(OUT, exist_ok=True)
    for path in (python_installer(), c_installer()):
        print(f"built {os.path.relpath(path, ROOT)} ({os.path.getsize(path)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
