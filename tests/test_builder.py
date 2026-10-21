#!/usr/bin/env python3
"""Tests for tools/builder.py: arguments, the wizard, dry runs and real builds."""

import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import builder  # noqa: E402

VERSION = "0.5.0"


def run_main(argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = builder.main(argv)
    return code, out.getvalue(), err.getvalue()


class ArgumentTests(unittest.TestCase):
    def test_list_shows_every_target(self):
        code, out, _ = run_main(["--list"])
        self.assertEqual(code, 0)
        for target in builder.TARGETS:
            self.assertIn(target, out)

    def test_no_arguments_without_terminal_fails_with_help(self):
        # Under the test runner stdin is not a terminal.
        code, _, err = run_main([])
        self.assertEqual(code, 2)
        self.assertIn("--target", err)

    def test_target_required_with_other_flags(self):
        code, _, err = run_main(["--out", "x"])
        self.assertEqual(code, 2)
        self.assertIn("choose a target", err)

    def test_bad_name_is_rejected(self):
        code, _, err = run_main(["--target", "c", "--name", "bad name", "--dry-run"])
        self.assertEqual(code, 1)
        self.assertIn("bad name", err)

    def test_name_validation(self):
        self.assertEqual(builder.validate_name("super_rct-1.2"), "super_rct-1.2")
        for bad in ("", "../x", "a/b", "-lead", "x" * 70):
            with self.subTest(name=bad), self.assertRaises(builder.BuildError):
                builder.validate_name(bad)


class DryRunTests(unittest.TestCase):
    def test_every_target_dry_run_creates_nothing(self):
        out = tempfile.mkdtemp()
        target_dir = os.path.join(out, "dist")
        for target in ("c", "installers", "py-nuitka"):
            with self.subTest(target=target):
                args = ["--target", target, "--out", target_dir, "--dry-run"]
                if target == "py-nuitka":
                    # Nuitka is not installed in the test environment; the plan
                    # still reports the missing tool before building anything.
                    with mock.patch.object(builder.importlib.util, "find_spec",
                                           return_value=None):
                        code, _, err = run_main(args)
                    self.assertEqual(code, 1)
                    self.assertIn("Nuitka is not installed", err)
                    continue
                code, out_text, _ = run_main(args)
                self.assertEqual(code, 0)
                self.assertIn("Dry run", out_text)
        self.assertFalse(os.path.exists(target_dir))
        shutil.rmtree(out, ignore_errors=True)


class WizardTests(unittest.TestCase):
    def test_wizard_dry_run_then_cancel(self):
        # target 1 (c), default output, default name, dry run yes, start no
        answers = ["1", "", "", "y", "n"]
        with mock.patch("builtins.input", side_effect=answers), \
                contextlib.redirect_stdout(io.StringIO()) as buf:
            code = builder.wizard()
        self.assertEqual(code, 0)
        self.assertIn("Cancelled", buf.getvalue())

    def test_wizard_rejects_bad_choice_and_name_then_retries(self):
        # bad choice 9, then 1 (c); bad name, then a good name; dry run; start
        answers = ["9", "1", "", "bad name", "tool", "y", "y"]
        with mock.patch("builtins.input", side_effect=answers), \
                contextlib.redirect_stdout(io.StringIO()) as buf:
            code = builder.wizard()
        self.assertEqual(code, 0)
        text = buf.getvalue()
        self.assertIn("Enter a number from 1 to", text)
        self.assertIn("bad name", text)
        self.assertIn("Dry run", text)
        self.assertIn("Copy the binary to", text)
        self.assertIn("tool", text)


class PlatformRefusalTests(unittest.TestCase):
    """Installers are refused off Linux; the C bot is built for Windows with gcc or ziglang."""

    def test_installers_refused_on_windows(self):
        out = tempfile.mkdtemp(prefix="srct-win-")
        try:
            with mock.patch.object(builder.sys, "platform", "win32"):
                code, _, err = run_main(["--target", "installers", "--out", out])
                self.assertEqual(code, 1)
                self.assertIn("Linux", err)
            self.assertFalse(os.path.exists(os.path.join(out, "super_rct")))
        finally:
            shutil.rmtree(out, ignore_errors=True)

    def test_windows_c_dry_run_links_winsock_and_iphlpapi(self):
        with mock.patch.object(builder.sys, "platform", "win32"), \
                mock.patch.object(builder, "windows_c_compiler", return_value=["gcc"]):
            plan = builder.Plan(target="c", out_dir=Path("/tmp/none"), name="super_rct-discord-c",
                                dry_run=True, protocol="discord")
            argv = next(step.run[0] for step in builder.build_steps(plan)
                        if isinstance(step.run, tuple))
        self.assertIn("-lws2_32", argv)
        self.assertIn("-liphlpapi", argv)
        self.assertIn("-DSRCT_PROTOCOL=2", argv)
        self.assertTrue(str(argv[argv.index("-o") + 1]).endswith("super_rct-discord-c.exe"))

    def test_windows_c_without_compiler_is_a_clear_error(self):
        out = tempfile.mkdtemp(prefix="srct-win-")
        try:
            with mock.patch.object(builder.sys, "platform", "win32"), \
                    mock.patch.object(builder, "windows_c_compiler", return_value=None):
                code, _, err = run_main(["--target", "c", "--out", out])
            self.assertEqual(code, 1)
            self.assertIn("C compiler", err)
            self.assertIn("ziglang", err)
        finally:
            shutil.rmtree(out, ignore_errors=True)


class ProtocolTests(unittest.TestCase):
    """The chosen protocol reaches the C build (make flag) and the Python copy."""

    def test_protocol_reaches_make_and_python_stage(self):
        plan = builder.Plan(target="c", out_dir=Path("/tmp/none"), name="t",
                            dry_run=True, protocol="discord")
        argv = next(step.run[0] for step in builder.build_steps(plan)
                    if isinstance(step.run, tuple))
        self.assertIn("PROTOCOL=discord", argv)
        with tempfile.TemporaryDirectory() as tmp:
            builder.stage_python_sources(Path(tmp), "telegram")
            text = (Path(tmp) / "build_protocol.py").read_text(encoding="utf-8")
        self.assertIn('PROTOCOL = "telegram"', text)

    def test_unknown_protocol_is_refused(self):
        with self.assertRaises(SystemExit) as ctx, contextlib.redirect_stderr(io.StringIO()):
            run_main(["--target", "c", "--protocol", "bogus", "--dry-run"])
        self.assertEqual(ctx.exception.code, 2)


class RealBuildTests(unittest.TestCase):
    """Builds real artifacts into a temporary folder and runs them."""

    @classmethod
    def setUpClass(cls):
        cls.out = tempfile.mkdtemp(prefix="srct-builder-")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.out, ignore_errors=True)

    def test_python_target_is_onefile(self):
        """The Python target is one Nuitka executable (--onefile), not a folder or archive."""
        plan = builder.Plan(target="py-nuitka", out_dir=Path(self.out), name="srct_py")
        with mock.patch.object(builder.importlib.util, "find_spec", return_value=object()):
            steps = builder.build_steps(plan)
        argv = next(step.run[0] for step in steps if isinstance(step.run, tuple))
        self.assertIn("--onefile", argv)
        self.assertIn("--output-filename=srct_py", argv)

    @unittest.skipIf(sys.platform == "win32", "the C bot is POSIX-only")
    def test_c_binary_runs(self):
        code, _, err = run_main(["--target", "c", "--out", self.out, "--name", "srct_c"])
        self.assertEqual(code, 0, err)
        exe = os.path.join(self.out, "srct_c")
        res = subprocess.run([exe, "--version"], capture_output=True, text=True, timeout=30)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertEqual(res.stdout.strip(), f"super_rct {VERSION}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
