#!/usr/bin/env python3
"""Super_RCT installer for Linux (Python build).

  python3 super_rct-linux.py install [--no-service]
  python3 super_rct-linux.py uninstall

install     extracts the bot into the current directory, asks for settings,
            and registers a systemd user service so it starts at login.
uninstall   stops and removes that service and the files install created.

Nothing is copied outside the current directory except the service unit in
~/.config/systemd/user. This file is never deleted. --no-service only extracts
and writes the settings file.
"""

import base64
import getpass
import io
import os
import shutil
import subprocess
import sys
import zipfile

PAYLOAD = ""  # replaced by tools/build_installers.py with the bot bundle (zip, base64)
ENV_FILE = "super_rct.env"
MANIFEST = "super_rct_install.txt"
UNIT_NAME = "super-rct.service"


def ask(prompt, secret=False, required=True):
    while True:
        use_getpass = secret and sys.stdin.isatty()
        value = (getpass.getpass(prompt) if use_getpass else input(prompt)).strip()
        if value or not required:
            return value
        print("  (required)")


def systemd_quote(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def run(argv):
    try:
        return subprocess.run(argv, capture_output=True, text=True).returncode
    except OSError:
        return -1


def unit_path():
    home = os.environ.get("HOME")
    if not home:
        raise SystemExit("error: HOME is not set; cannot find the systemd user directory")
    return os.path.join(home, ".config", "systemd", "user", UNIT_NAME)


def install(no_service):
    here = os.getcwd()
    if os.path.exists(MANIFEST):
        print(f"Already installed here ({MANIFEST} exists). Run uninstall first.")
        return 1
    if not PAYLOAD:
        print("error: this installer has no bot payload; build it with tools/build_installers.py")
        return 1

    names = zipfile.ZipFile(io.BytesIO(base64.b64decode(PAYLOAD))).namelist()
    clash = [n for n in names + [ENV_FILE] if os.path.exists(os.path.join(here, n))]
    if clash:
        print("Refusing to overwrite existing files: " + ", ".join(clash))
        return 1

    print("Super_RCT setup. Secrets are typed without echo and saved to "
          f"{ENV_FILE} (readable only by you).")
    values = {}
    values["SUPER_RCT_TOKEN"] = ask("Telegram bot token: ", secret=True)
    values["SUPER_RCT_CHAT_IDS"] = ask("Allowed Telegram chat IDs (comma-separated): ")
    while True:
        password = ask("Login password (8-128 characters): ", secret=True)
        if ask("Repeat password: ", secret=True) == password:
            break
        print("  Passwords differ, try again.")
    values["SUPER_RCT_PASSWORD"] = password
    discord = ask("Discord bot token (Enter to skip): ", secret=True, required=False)
    if discord:
        values["SUPER_RCT_DISCORD_TOKEN"] = discord
        values["SUPER_RCT_DISCORD_CHANNEL_IDS"] = ask("Allowed Discord channel IDs: ")

    created = []
    with zipfile.ZipFile(io.BytesIO(base64.b64decode(PAYLOAD))) as bundle:
        bundle.extractall(here)
    created.extend(names)

    env_path = os.path.join(here, ENV_FILE)
    fd = os.open(env_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        for key, value in values.items():
            if "\n" in value or "\r" in value:
                raise SystemExit(f"error: {key} must not contain line breaks")
            f.write(f"{key}={value}\n")
    created.append(ENV_FILE)
    print(f"Extracted {len(names)} bot files and wrote {ENV_FILE}.")

    unit = None
    if not no_service:
        unit = unit_path()
        os.makedirs(os.path.dirname(unit), exist_ok=True)
        with open(unit, "w", encoding="utf-8") as f:
            f.write("[Unit]\nDescription=Super_RCT bot\nAfter=network-online.target\n\n"
                    "[Service]\n"
                    f"WorkingDirectory={systemd_quote(here)}\n"
                    f"EnvironmentFile={systemd_quote(env_path)}\n"
                    f"ExecStart={systemd_quote(sys.executable)} "
                    f"{systemd_quote(os.path.join(here, 'super_rct.py'))}\n"
                    "Environment=PYTHONDONTWRITEBYTECODE=1\n"
                    "Restart=on-failure\nRestartSec=10\n\n"
                    "[Install]\nWantedBy=default.target\n")
        if run(["systemctl", "--user", "daemon-reload"]) != 0 or \
                run(["systemctl", "--user", "enable", "--now", UNIT_NAME]) != 0:
            print("warning: could not enable the systemd user service here. The bot files "
                  "are in place; start it with the settings in " + ENV_FILE + ".")
        print(f"Service file written: {unit}")

    with open(os.path.join(here, MANIFEST), "w", encoding="utf-8") as f:
        for name in created:
            f.write(f"file {name}\n")
        if unit:
            f.write(f"unit {unit}\n")
    print("Done. To remove everything later: python3 " + os.path.basename(__file__) + " uninstall")
    return 0


def uninstall():
    here = os.getcwd()
    manifest = os.path.join(here, MANIFEST)
    if not os.path.exists(manifest):
        print("Nothing to uninstall here (no " + MANIFEST + ").")
        return 0
    with open(manifest, encoding="utf-8") as f:
        entries = [line.rstrip("\n").split(" ", 1) for line in f if line.strip()]

    for kind, path in entries:
        if kind == "unit" and os.path.exists(path):
            run(["systemctl", "--user", "disable", "--now", UNIT_NAME])
            os.remove(path)
            run(["systemctl", "--user", "daemon-reload"])
            print(f"Removed service {path}")
    for kind, name in entries:
        if kind == "file":
            target = os.path.join(here, name)
            if os.path.isfile(target):
                os.remove(target)
                print(f"Removed {name}")
    cache = os.path.join(here, "__pycache__")
    if os.path.isdir(cache) and all(n.endswith(".pyc") for n in os.listdir(cache)):
        shutil.rmtree(cache)
        print("Removed __pycache__")
    os.remove(manifest)
    print("Uninstalled. This installer file was left in place.")
    return 0


def main(argv):
    args = argv[1:]
    if args == ["install"]:
        return install(no_service=False)
    if args == ["install", "--no-service"]:
        return install(no_service=True)
    if args == ["uninstall"]:
        return uninstall()
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
