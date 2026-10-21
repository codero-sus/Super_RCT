#!/usr/bin/env python3
"""Builds the Linux installers into build/installers/ (not committed).

  python3 tools/build_installers.py                     both protocols
  python3 tools/build_installers.py --protocol discord  one protocol

For each protocol there are two installers, one per language:
  super_rct-<protocol>-linux.py   Python build, bot bundle embedded (zip, base64)
  super_rct-<protocol>-linux      C build, release bot binary embedded (base64)

Each installer extracts its embedded bot into the directory it is run from.
The C installer is compiled with cc. The bot inside is built for the protocol
named in the file name, so each installer serves only that protocol.
"""

import argparse
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
PY_BUNDLE = ["super_rct.py", "monitor.py", "discord_transport.py", "commands_table.py",
             "build_protocol.py"]
PROTOCOLS = ("telegram", "discord")


def python_installer(protocol):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for name in PY_BUNDLE:
            if name == "build_protocol.py":
                z.writestr(name, f'PROTOCOL = "{protocol}"\n')
            else:
                z.write(os.path.join(ROOT, "src", name), name)
    payload = base64.b64encode(buf.getvalue()).decode()

    with open(os.path.join(ROOT, "src", "install", "linux.py"), encoding="utf-8") as f:
        src = f.read()
    src, count = re.subn(r'^PAYLOAD = ""', f'PAYLOAD = "{payload}"', src, count=1, flags=re.M)
    assert count == 1, "payload placeholder not found in linux.py"

    dest = os.path.join(OUT, f"super_rct-{protocol}-linux.py")
    with open(dest, "w", encoding="utf-8") as f:
        f.write(src)
    os.chmod(dest, os.stat(dest).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return dest


def c_installer(protocol):
    binary = os.path.join(ROOT, "build", "super_rct")
    subprocess.run(["make", "-C", ROOT, "release", f"PROTOCOL={protocol}"], check=True,
                   stdout=subprocess.DEVNULL)
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

    src_path = os.path.join(OUT, f"super_rct-{protocol}-linux.c")
    with open(src_path, "w", encoding="utf-8") as f:
        f.write(src)
    dest = os.path.join(OUT, f"super_rct-{protocol}-linux")
    subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Wshadow",
                    "-Wformat=2", "-s", "-o", dest, src_path], check=True)
    os.remove(src_path)
    return dest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("--protocol", choices=("both",) + PROTOCOLS, default="both")
    args = parser.parse_args(argv)
    protocols = PROTOCOLS if args.protocol == "both" else (args.protocol,)

    os.makedirs(OUT, exist_ok=True)
    for protocol in protocols:
        for path in (python_installer(protocol), c_installer(protocol)):
            print(f"built {os.path.relpath(path, ROOT)} ({os.path.getsize(path)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
