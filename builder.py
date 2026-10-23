#!/usr/bin/env python3
"""Build the Super_RCT Android app (debug APK) and optionally install it on your phone.

    python builder.py                  enter the secrets once (first run), build the APK
    python builder.py --install        also install it on the USB-connected phone and start it
    python builder.py --install --serial <id>   pick one phone when several are connected
    python builder.py --no-build --install      install the last built APK, no rebuild
    python builder.py --test           run the core unit tests first
    python builder.py --reset-secrets  enter the token, chat IDs and password again
    python builder.py --check          only report which tools and secrets were found

The bot token, the login password and the allowed chat IDs are compiled into the APK.
They are stored only in android\\app\\secrets.properties, which git ignores. Anyone who
has the APK can read them, so do not share the APK.

Run setup.bat once first on Windows. Needs Python 3.8+ and nothing else
(standard library only). Paths are relative to this file.
"""

import argparse
import getpass
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
ANDROID = ROOT / "android"
DIST = ROOT / "dist"
APP_BUILD = ANDROID / "app" / "build.gradle"
SECRETS = ANDROID / "app" / "secrets.properties"
APK_OUT = ANDROID / "app" / "build" / "outputs" / "apk" / "debug" / "app-debug.apk"
SERVICE = "rct.android/.BotService"
WIN = os.name == "nt"

# Same limits as the Java core (rct.core.Config).
TOKEN_RE = re.compile(r"[0-9]{1,20}:[A-Za-z0-9_-]{10,200}")
CHAT_RE = re.compile(r"-?[0-9]{1,20}")
MAX_CHATS = 32
MIN_PASSWORD, MAX_PASSWORD = 8, 128
MIN_SESSION, MAX_SESSION, DEFAULT_SESSION = 5, 86400, "3600"
REQUIRED_KEYS = ("SUPER_RCT_TOKEN", "SUPER_RCT_CHAT_IDS", "SUPER_RCT_PASSWORD")


class BuildError(Exception):
    pass


def say(text=""):
    print(text, flush=True)


# ---- tools -------------------------------------------------------------------

def find_java():
    home = os.environ.get("JAVA_HOME")
    if home:
        exe = Path(home) / "bin" / ("java.exe" if WIN else "java")
        if exe.exists():
            return str(exe)
    return shutil.which("java")


def find_sdk():
    for key in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        value = os.environ.get(key)
        if value and Path(value).is_dir():
            return Path(value)
    if WIN:
        local = os.environ.get("LOCALAPPDATA")
        if local and (Path(local) / "Android" / "Sdk").is_dir():
            return Path(local) / "Android" / "Sdk"
    return None


def find_adb(sdk):
    if sdk:
        exe = sdk / "platform-tools" / ("adb.exe" if WIN else "adb")
        if exe.exists():
            return str(exe)
    return shutil.which("adb")


def detect():
    sdk = find_sdk()
    wrapper = (ANDROID / ("gradlew.bat" if WIN else "gradlew")).exists()
    return {
        "java": find_java(),
        "sdk": sdk,
        "adb": find_adb(sdk),
        "wrapper": wrapper,
        "gradle": None if wrapper else shutil.which("gradle"),
    }


def report(info):
    rows = [
        ("Java (JDK 17)", info["java"]),
        ("Android SDK", str(info["sdk"]) if info["sdk"] else None),
        ("Gradle", ("android\\gradlew.bat" if WIN else "android/gradlew")
         if info["wrapper"] else info["gradle"]),
        ("adb (for --install)", info["adb"]),
    ]
    for name, found in rows:
        say(f"  {name:<20} {'OK  ' + found if found else 'MISSING'}")
    say(f"  {'Secrets':<20} {'OK  (values hidden)' if secrets_complete() else 'not entered yet'}")


def required_missing(info):
    missing = []
    if not info["java"]:
        missing.append("Java 17")
    if not info["sdk"]:
        missing.append("Android SDK")
    if not info["wrapper"] and not info["gradle"]:
        missing.append("Gradle")
    return missing


def read_version():
    match = re.search(r"versionName\s+'([^']+)'", APP_BUILD.read_text(encoding="utf-8"))
    return match.group(1) if match else "0.0.0"


def escape_properties(path):
    """Escapes a path for android/local.properties (backslashes and colons)."""
    return str(path).replace("\\", "\\\\").replace(":", "\\:")


def ensure_local_properties(sdk):
    target = ANDROID / "local.properties"
    if target.exists() or sdk is None:
        return
    target.write_text(f"sdk.dir={escape_properties(sdk)}\n", encoding="utf-8")
    say(f"Wrote {target.relative_to(ROOT)} (points Gradle at the Android SDK)")


def run(argv, cwd, env=None):
    """Runs a command, showing its output live. Returns the exit code."""
    say("$ " + " ".join(argv))
    return subprocess.call(argv, cwd=str(cwd), env=env, shell=WIN)


def gradle_argv(info, *tasks):
    head = (["gradlew.bat"] if WIN else ["./gradlew"]) if info["wrapper"] else ["gradle"]
    return head + ["--console=plain", *tasks]


# ---- secrets ------------------------------------------------------------------

def unescape_properties(text):
    """Reads a value the way Java's Properties.load does: \\\\ \\uXXXX and an escaped leading space."""
    out = []
    index = 0
    while index < len(text):
        char = text[index]
        if char == "\\" and index + 1 < len(text):
            nxt = text[index + 1]
            if nxt == "u" and index + 6 <= len(text):
                # A character outside the BMP arrives as a surrogate pair.
                out.append(chr(int(text[index + 2:index + 6], 16)))
                index += 6
                continue
            out.append(nxt)
            index += 2
            continue
        out.append(char)
        index += 1
    return "".join(out).encode("utf-16", "surrogatepass").decode("utf-16")


def load_properties(path):
    """Reads the simple key=value file that write_secrets creates."""
    values = {}
    for raw in path.read_text(encoding="ascii").splitlines():
        line = raw.strip()
        if not line or line.startswith(("#", "!")):
            continue
        key, _, value = line.partition("=")
        values[key.strip()] = unescape_properties(value)
    return values


def secrets_complete():
    if not SECRETS.exists():
        return False
    try:
        values = load_properties(SECRETS)
    except (OSError, UnicodeDecodeError):
        return False
    return all(values.get(key) for key in REQUIRED_KEYS)


def prop_value(text):
    """Escapes a value for a .properties file. Non-ASCII becomes \\uXXXX, so it reads back exactly."""
    data = text.encode("utf-16-be")
    out = []
    for i in range(0, len(data), 2):
        unit = int.from_bytes(data[i:i + 2], "big")
        ch = chr(unit)
        if unit == 0x20 and i == 0:
            out.append("\\ ")  # a leading space would be dropped when read back
        elif ch in "#!" and i == 0:
            out.append("\\" + ch)  # would start a comment when read back
        elif ch == "\\":
            out.append("\\\\")
        elif 0x20 <= unit <= 0x7E:
            out.append(ch)
        else:
            out.append("\\u%04x" % unit)
    return "".join(out)


def check_token(value):
    return bool(TOKEN_RE.fullmatch(value))


def check_chats(value):
    ids = [part.strip() for part in value.split(",") if part.strip()]
    return 0 < len(ids) <= MAX_CHATS and all(CHAT_RE.fullmatch(i) for i in ids)


def check_password(value):
    return MIN_PASSWORD <= len(value.encode("utf-8")) <= MAX_PASSWORD


def check_session(value):
    return value.isdigit() and MIN_SESSION <= int(value) <= MAX_SESSION


def ask(prompt, check, hint, hidden=False, default=None):
    """Asks until the answer passes `check`. Hidden answers are not echoed."""
    while True:
        raw = getpass.getpass(prompt) if hidden else input(prompt)
        value = raw if hidden else raw.strip()
        if not value and default is not None:
            return default
        if check(value):
            return value
        say(f"  not valid ({hint}). Try again.")


def prompt_secrets():
    if not sys.stdin.isatty():
        raise BuildError("the secrets are not entered yet. Run python builder.py in a "
                         "terminal once; it asks for them.")
    say("Enter the values to compile into the APK. They are saved only in "
        "android\\app\\secrets.properties (git-ignored). Typing is hidden for the token and password.")
    token = ask("Bot token (from @BotFather): ", check_token,
                "like 123456789:AAE...", hidden=True).strip()
    chats = ask("Allowed chat IDs (comma-separated): ", check_chats,
                "numbers separated by commas")
    chats = ",".join(part.strip() for part in chats.split(",") if part.strip())
    while True:
        password = ask("Login password (8-128 characters): ", check_password,
                       "8-128 characters", hidden=True)
        again = getpass.getpass("Type the password again: ")
        if again == password:
            break
        say("  the two passwords differ. Try again.")
    session = ask(f"Idle session seconds [{DEFAULT_SESSION}]: ", check_session,
                  f"{MIN_SESSION}-{MAX_SESSION}", default=DEFAULT_SESSION)
    write_secrets(token, chats, password, session)


def write_secrets(token, chats, password, session):
    SECRETS.parent.mkdir(parents=True, exist_ok=True)
    text = (
        "# Written by builder.py. Compiled into the APK. Do not commit this file.\n"
        f"SUPER_RCT_TOKEN={prop_value(token)}\n"
        f"SUPER_RCT_CHAT_IDS={prop_value(chats)}\n"
        f"SUPER_RCT_PASSWORD={prop_value(password)}\n"
        f"SUPER_RCT_SESSION_SECONDS={prop_value(session)}\n"
    )
    SECRETS.write_text(text, encoding="ascii")
    say(f"Saved {SECRETS.relative_to(ROOT)}")


def ensure_secrets(reset):
    if SECRETS.exists() and not reset and secrets_complete():
        return
    if reset:
        say("Re-entering the secrets.")
    elif SECRETS.exists():
        say("The secrets file is incomplete; entering the values again.")
    prompt_secrets()


# ---- build and install --------------------------------------------------------

def build(info, env, test):
    if test:
        if run(gradle_argv(info, ":core:test"), ANDROID, env) != 0:
            raise BuildError("the core unit tests failed (see the output above)")
    if run(gradle_argv(info, ":app:assembleDebug"), ANDROID, env) != 0:
        raise BuildError("the APK build failed (see the output above)")
    if not APK_OUT.exists():
        raise BuildError(f"the build finished but {APK_OUT} is missing")
    DIST.mkdir(parents=True, exist_ok=True)
    target = DIST / f"super_rct-android-{read_version()}-debug.apk"
    shutil.copy2(APK_OUT, target)
    return target


def pick_phone(adb, serial):
    listing = subprocess.run([adb, "devices"], capture_output=True, text=True).stdout
    rows = [line.split("\t") for line in listing.splitlines() if "\t" in line]
    ready = [row[0] for row in rows if len(row) > 1 and row[1].strip() == "device"]
    if any(len(row) > 1 and row[1].strip() == "unauthorized" for row in rows):
        say("A phone is connected but not authorised: unlock it and accept the "
            "'Allow USB debugging' prompt.")
    if serial:
        return serial
    if len(ready) == 1:
        return ready[0]
    if not ready:
        raise BuildError("no phone found. Connect it by USB, turn on USB debugging "
                         "(Settings > About phone > tap Build number 7 times, then "
                         "Developer options > USB debugging), and accept the prompt.")
    raise BuildError("more than one phone is connected; choose one with --serial <id>. "
                     "Connected: " + ", ".join(ready))


def install(adb, apk, serial):
    if not adb:
        raise BuildError("adb not found. Run setup.bat (it installs platform-tools).")
    target = pick_phone(adb, serial)
    if run([adb, "-s", target, "install", "-r", str(apk)], ROOT) != 0:
        raise BuildError("installing on the phone failed (see the output above)")
    # No screen to open: start the silent service directly.
    if run([adb, "-s", target, "shell", "am", "start-foreground-service", "-n", SERVICE], ROOT) != 0:
        say("Installed, but the service did not start. Reboot the phone; it starts at boot.")
    else:
        say(f"Installed on {target} and started the silent service.")
    say("It starts again after every reboot. Watch its log with:  adb -s "
        f"{target} logcat -s SuperRct")


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Build the Super_RCT Android APK (silent service) and optionally install it.")
    parser.add_argument("--test", action="store_true", help="run the core unit tests first")
    parser.add_argument("--install", action="store_true", help="install on the USB-connected phone")
    parser.add_argument("--serial", help="phone id from 'adb devices' when several are connected")
    parser.add_argument("--no-build", action="store_true", help="skip the build (use the last APK)")
    parser.add_argument("--reset-secrets", action="store_true",
                        help="enter the token, chat IDs and password again")
    parser.add_argument("--check", action="store_true", help="only report what was found")
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    info = detect()
    say("Tools:")
    report(info)
    missing = required_missing(info)
    if args.check:
        if missing:
            say("Missing: " + ", ".join(missing) + ". Run setup.bat.")
            return 2
        say("All required tools found.")
        return 0
    if missing:
        say("Missing: " + ", ".join(missing) + ". Run setup.bat first.")
        return 2

    try:
        if args.no_build:
            if not APK_OUT.exists() and not any(DIST.glob("*.apk")):
                raise BuildError("no APK yet. Build first (python builder.py).")
            apk = APK_OUT if APK_OUT.exists() else max(DIST.glob("*.apk"), key=os.path.getmtime)
        else:
            ensure_secrets(args.reset_secrets)
            env = os.environ.copy()
            env["ANDROID_HOME"] = str(info["sdk"])
            ensure_local_properties(info["sdk"])
            apk = build(info, env, args.test)
            say(f"APK: {apk}")
            say("This APK contains your token and password. Do not share it.")
        if args.install:
            install(info["adb"], apk, args.serial)
    except BuildError as err:
        say(f"error: {err}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
