#!/usr/bin/env python3
"""Super_RCT builder: build an executable with a terminal wizard or with arguments.

    python3 tools/builder.py              step-by-step wizard (asks each question)
    python3 tools/builder.py --target c   build without prompts

Run `python3 tools/builder.py --help` for every option. Nothing needs to be
read from README.md first. Paths are relative to the repository root.
"""

import argparse
import importlib.util
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import zipapp
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
DEFAULT_OUT = "build/dist"
DEFAULT_NAME = "super_rct"
NAME_RE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}")

# Python files the bot needs at run time (super_rct.py imports the others).
PY_FILES = ("super_rct.py", "monitor.py", "commands_table.py", "discord_transport.py")

TARGETS = {
    "c": "C binary: smallest, no Python needed (make release)",
    "py-zipapp": "Python archive: one file, runs with python3, no extra tools",
    "py-nuitka": "Python native executable via Nuitka (needs Nuitka and C headers)",
    "installers": "Linux installers, C and Python: each embeds the bot and sets it up",
}


class BuildError(Exception):
    pass


@dataclass
class Plan:
    target: str
    out_dir: Path
    name: str
    dry_run: bool = False
    notes: list = field(default_factory=list)


@dataclass
class Step:
    text: str
    run: object  # argv list (run as a subprocess) or a callable taking the Plan


def validate_name(name):
    if not NAME_RE.fullmatch(name):
        raise BuildError(f"bad name {name!r}: use letters, digits, '_', '-' or '.'")
    return name


def resolve_out(text):
    path = Path(text)
    return path if path.is_absolute() else ROOT / path


def stage_python_sources(dest):
    """Copies the bot's Python files into `dest` and returns that path."""
    dest.mkdir(parents=True, exist_ok=True)
    for name in PY_FILES:
        shutil.copy2(SRC / name, dest / name)
    return dest


def make_executable(path):
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def zipapp_step(plan):
    stage = Path(tempfile.mkdtemp(prefix="srct-stage-"))
    try:
        stage_python_sources(stage)
        (stage / "__main__.py").write_text(
            "import sys\nfrom super_rct import main\n\nsys.exit(main(sys.argv))\n",
            encoding="utf-8")
        plan.out_dir.mkdir(parents=True, exist_ok=True)
        target = plan.out_dir / f"{plan.name}.pyz"
        zipapp.create_archive(stage, target, interpreter="/usr/bin/env python3",
                              compressed=True)
        make_executable(target)
    finally:
        shutil.rmtree(stage, ignore_errors=True)


def c_copy_step(plan):
    plan.out_dir.mkdir(parents=True, exist_ok=True)
    built = ROOT / "build" / "super_rct"
    if not built.exists():
        raise BuildError("make release did not produce build/super_rct")
    target = plan.out_dir / plan.name
    shutil.copy2(built, target)
    make_executable(target)


def installers_step(plan):
    plan.out_dir.mkdir(parents=True, exist_ok=True)
    for built in (ROOT / "build" / "installers").glob("super_rct-linux*"):
        shutil.copy2(built, plan.out_dir / built.name)


def nuitka_steps(plan):
    if importlib.util.find_spec("nuitka") is None:
        raise BuildError(
            "Nuitka is not installed. Install it in a virtual environment with:\n"
            f"    {sys.executable} -m venv build/nuitka-env\n"
            "    build/nuitka-env/bin/pip install nuitka\n"
            "then run this builder with that environment's python.")
    stage = ROOT / "build" / "nuitka-stage"
    argv = [sys.executable, "-m", "nuitka", "--onefile", "--assume-yes-for-downloads",
            f"--output-dir={plan.out_dir}", f"--output-filename={plan.name}",
            "super_rct.py"]
    return [
        Step(f"Copy the bot sources to {stage}", lambda p: stage_python_sources(stage)),
        Step("Compile with Nuitka", (argv, stage)),
    ]


def build_steps(plan):
    """Returns the ordered steps for `plan`. Raises BuildError for bad combinations."""
    if plan.target == "c":
        return [Step("Compile the C bot (make release)", (["make", "release"], ROOT)),
                Step(f"Copy the binary to {plan.out_dir / plan.name}", c_copy_step)]
    if plan.target == "py-zipapp":
        return [Step(f"Create {plan.out_dir / (plan.name + '.pyz')}", zipapp_step)]
    if plan.target == "py-nuitka":
        return nuitka_steps(plan)
    if plan.target == "installers":
        if sys.platform != "linux":
            plan.notes.append("The installers are for Linux; build them on Linux.")
        return [Step("Build the Linux installers (tools/build_installers.py)",
                     ([sys.executable, str(ROOT / "tools" / "build_installers.py")], ROOT)),
                Step(f"Copy them to {plan.out_dir}", installers_step)]
    raise BuildError(f"unknown target {plan.target!r}")


def describe(step):
    if isinstance(step.run, tuple):
        argv, cwd = step.run
        return f"{step.text}\n    $ {' '.join(argv)}   (in {cwd})"
    return step.text


def execute(plan):
    steps = build_steps(plan)
    for note in plan.notes:
        print(f"note: {note}")
    if plan.dry_run:
        print("Dry run. These steps would run:")
        for step in steps:
            print(f"  - {describe(step)}")
        return 0
    for number, step in enumerate(steps, 1):
        print(f"[{number}/{len(steps)}] {step.text}")
        try:
            if isinstance(step.run, tuple):
                argv, cwd = step.run
                result = subprocess.run(argv, cwd=cwd, check=False)
                if result.returncode != 0:
                    raise BuildError(f"command failed with exit code {result.returncode}: "
                                     + " ".join(argv))
            else:
                step.run(plan)
        except OSError as err:
            raise BuildError(f"{step.text}: {err}") from None
    print(f"Done. Output is in {plan.out_dir}")
    return 0


# ---- wizard ---------------------------------------------------------------

def ask(prompt, default=None, validator=None):
    while True:
        suffix = f" [{default}]" if default is not None else ""
        raw = input(f"{prompt}{suffix}: ").strip()
        if not raw and default is not None:
            raw = default
        if not raw:
            print("  Please enter a value.")
            continue
        if validator:
            try:
                return validator(raw)
            except BuildError as err:
                print(f"  {err}")
                continue
        return raw


def ask_choice(prompt, options, default_index=1):
    print(prompt)
    for number, (_, label) in enumerate(options, 1):
        print(f"  {number}) {label}")
    while True:
        raw = input(f"Choose 1-{len(options)} [{default_index}]: ").strip() or str(default_index)
        if raw.isdigit() and 1 <= int(raw) <= len(options):
            return options[int(raw) - 1][0]
        print(f"  Enter a number from 1 to {len(options)}.")


def ask_yes(prompt, default=False):
    hint = "Y/n" if default else "y/N"
    while True:
        raw = input(f"{prompt} [{hint}]: ").strip().lower()
        if not raw:
            return default
        if raw in ("y", "yes"):
            return True
        if raw in ("n", "no"):
            return False
        print("  Answer y or n.")


def wizard():
    print("Super_RCT builder: answer each question. Press Ctrl+C at any time to stop.\n")
    print("Step 1 of 4: what do you want to build?")
    target = ask_choice("", list(TARGETS.items()))
    print("\nStep 2 of 4: where should the output go?")
    out_text = ask("Output folder (relative to the repository root)", DEFAULT_OUT)
    print("\nStep 3 of 4: what should the file be called?")
    name = ask("File name", DEFAULT_NAME, validate_name)
    print("\nStep 4 of 4: dry run only (show the steps without building)?")
    dry = ask_yes("Dry run", default=False)
    plan = Plan(target=target, out_dir=resolve_out(out_text), name=name, dry_run=dry)

    print("\nSummary")
    print(f"  target:  {target}")
    print(f"  output:  {plan.out_dir}")
    print(f"  name:    {name}")
    print(f"  mode:    {'dry run' if dry else 'build'}")
    if not ask_yes("Start now", default=True):
        print("Cancelled. Nothing was built.")
        return 0
    print()
    return execute(plan)


# ---- command line ---------------------------------------------------------

def build_parser():
    parser = argparse.ArgumentParser(
        prog="tools/builder.py",
        description="Build a Super_RCT executable. With no arguments on a terminal, "
                    "a step-by-step wizard starts.",
        epilog="targets:\n" + "\n".join(f"  {k:<11} {v}" for k, v in TARGETS.items()),
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--target", choices=list(TARGETS), help="what to build")
    parser.add_argument("--out", default=DEFAULT_OUT,
                        help=f"output folder, relative to the repository root (default {DEFAULT_OUT})")
    parser.add_argument("--name", default=DEFAULT_NAME,
                        help=f"output file name (default {DEFAULT_NAME})")
    parser.add_argument("--dry-run", action="store_true",
                        help="print the steps without building")
    parser.add_argument("--wizard", action="store_true",
                        help="ask every question step by step, even when arguments are given")
    parser.add_argument("--list", action="store_true", help="list the targets and exit")
    return parser


def parse_args(argv):
    return build_parser().parse_args(argv)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    args = parse_args(argv) if argv else None
    try:
        if args is None and sys.stdin.isatty():
            return wizard()
        if args is None:
            print("No arguments given and stdin is not a terminal.\n", file=sys.stderr)
            build_parser().print_help(sys.stderr)
            return 2
        if args.list:
            for key, label in TARGETS.items():
                print(f"{key:<11} {label}")
            return 0
        if args.wizard:
            return wizard()
        if not args.target:
            print("error: choose a target with --target (see --list), or run with no "
                  "arguments for the wizard", file=sys.stderr)
            return 2
        plan = Plan(target=args.target, out_dir=resolve_out(args.out),
                    name=validate_name(args.name), dry_run=args.dry_run)
        return execute(plan)
    except BuildError as err:
        print(f"error: {err}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nStopped.", file=sys.stderr)
        return 130
    except EOFError:
        print("\nNo more input. Stopped.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
