#!/usr/bin/env python3
"""Build the Super_RCT Android app (debug APK) and optionally install it on your phone.

    python builder.py                 build the APK and copy it to dist\\
    python builder.py --test          run the core unit tests first
    python builder.py --install       also install it on the phone connected by USB
    python builder.py --install --serial <id>   pick one phone when several are connected
    python builder.py --no-build --install      install the last built APK, no rebuild
    python builder.py --check         only report which tools were found

Run setup.bat once first on Windows. Needs Python 3.8+ and nothing else
(standard library only). Paths are relative to this file.
"""

import argparse
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
APK_OUT = ANDROID / "app" / "build" / "outputs" / "apk" / "debug" / "app-debug.apk"
WIN = os.name == "nt"


class BuildError(Exception):
    pass


def say(text=""):
    print(text, flush=True)


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
    if info["wrapper"]:
        head = ["gradlew.bat"] if WIN else ["./gradlew"]
    else:
        head = ["gradle"]
    return head + ["--console=plain", *tasks]


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


def install(adb, apk, serial):
    if not adb:
        raise BuildError("adb not found. Run setup.bat (it installs platform-tools).")
    listing = subprocess.run([adb, "devices"], capture_output=True, text=True).stdout
    rows = [line.split("\t") for line in listing.splitlines() if "\t" in line]
    ready = [row[0] for row in rows if len(row) > 1 and row[1].strip() == "device"]
    if any(len(row) > 1 and row[1].strip() == "unauthorized" for row in rows):
        say("A phone is connected but not authorised: unlock it and accept the "
            "'Allow USB debugging' prompt.")
    if serial:
        target = serial
    elif len(ready) == 1:
        target = ready[0]
    elif not ready:
        raise BuildError("no phone found. Connect it by USB, turn on USB debugging "
                         "(Settings > About phone > tap Build number 7 times, then "
                         "Developer options > USB debugging), and accept the prompt.")
    else:
        raise BuildError("more than one phone is connected; choose one with "
                         "--serial <id>. Connected: " + ", ".join(ready))
    if run([adb, "-s", target, "install", "-r", str(apk)], ROOT) != 0:
        raise BuildError("installing on the phone failed (see the output above)")
    say(f"Installed on {target}. Open Super_RCT on the phone.")


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Build the Super_RCT Android APK (debug) and optionally install it.")
    parser.add_argument("--test", action="store_true", help="run the core unit tests first")
    parser.add_argument("--install", action="store_true", help="install on the USB-connected phone")
    parser.add_argument("--serial", help="phone id from 'adb devices' when several are connected")
    parser.add_argument("--no-build", action="store_true", help="skip the build (use the last APK)")
    parser.add_argument("--check", action="store_true", help="only report the tools that were found")
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
        env = os.environ.copy()
        env["ANDROID_HOME"] = str(info["sdk"])
        ensure_local_properties(info["sdk"])
        if args.no_build:
            if not APK_OUT.exists() and not any(DIST.glob("*.apk")):
                raise BuildError("no APK yet. Build first (python builder.py).")
            apk = APK_OUT if APK_OUT.exists() else max(DIST.glob("*.apk"), key=os.path.getmtime)
        else:
            apk = build(info, env, args.test)
        say(f"APK: {apk}")
        if args.install:
            install(info["adb"], apk, args.serial)
    except BuildError as err:
        say(f"error: {err}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
