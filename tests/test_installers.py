#!/usr/bin/env python3
"""Tests for the Linux installers (C and Python builds).

Builds the installers with tools/build_installers.py, then runs each one in a
scratch directory with a scratch HOME: install (no service), run the extracted
bot, refuse a second install, uninstall. Requires make and cc, like the other
tests. The service-enable step is not run here, so no real systemd service is
touched.
"""

import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INSTALLERS = os.path.join(ROOT, "build", "installers")
VERSION = "0.5.0"
ANSWERS = (
    "123456789:TEST_token_for_mock_only\n"  # Telegram token
    "42, 7\n"                               # chat IDs
    "first password try\n"                  # password
    "typo in the repeat\n"                  # repeat differs, so the password is asked again
    "correct horse battery\n"               # password
    "correct horse battery\n"               # repeat
    "\n"                                    # Discord token: skip
)


class InstallerCases:
    """Shared cases. Subclasses set `installer` and `bot` (argv prefixes)."""

    installer = None
    bot = None

    @classmethod
    def setUpClass(cls):
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "build_installers.py")],
                       check=True, capture_output=True)

    def setUp(self):
        self.work = tempfile.mkdtemp()
        self.home = tempfile.mkdtemp()
        self.env = dict(os.environ, HOME=self.home, PYTHONDONTWRITEBYTECODE="1")

    def tearDown(self):
        shutil.rmtree(self.work, ignore_errors=True)
        shutil.rmtree(self.home, ignore_errors=True)

    def run_installer(self, *args, stdin=""):
        return subprocess.run(self.installer + list(args), cwd=self.work, env=self.env,
                              input=stdin, capture_output=True, text=True, timeout=60)

    def test_install_run_and_uninstall(self):
        res = self.run_installer("install", "--no-service", stdin=ANSWERS)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)

        env_path = os.path.join(self.work, "super_rct.env")
        self.assertEqual(stat.S_IMODE(os.stat(env_path).st_mode), 0o600)
        with open(env_path, encoding="utf-8") as f:
            settings = f.read()
        self.assertIn("SUPER_RCT_PASSWORD=correct horse battery\n", settings)
        self.assertIn("SUPER_RCT_CHAT_IDS=42, 7\n", settings)
        self.assertNotIn("SUPER_RCT_DISCORD_TOKEN", settings)

        # The extracted bot runs from the directory it was installed into.
        ver = subprocess.run(self.bot + ["--version"], cwd=self.work, env=self.env,
                             capture_output=True, text=True, timeout=30)
        self.assertEqual(ver.returncode, 0, ver.stderr)
        self.assertEqual(ver.stdout.strip(), f"super_rct {VERSION}")

        # A second install is refused while the first is present.
        again = self.run_installer("install", "--no-service", stdin=ANSWERS)
        self.assertNotEqual(again.returncode, 0)
        self.assertIn("Already installed", again.stdout)

        res = self.run_installer("uninstall")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        leftovers = sorted(os.listdir(self.work))
        self.assertEqual(leftovers, [], leftovers)  # bot, settings and cache removed
        self.assertTrue(os.path.exists(self.installer[-1]))  # the installer itself stays


class CInstallerTests(InstallerCases, unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.installer = [os.path.join(INSTALLERS, "super_rct-linux")]
        cls.bot = ["./super_rct"]


class PythonInstallerTests(InstallerCases, unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        cls.installer = [sys.executable, os.path.join(INSTALLERS, "super_rct-linux.py")]
        cls.bot = [sys.executable, "super_rct.py"]


if __name__ == "__main__":
    unittest.main(verbosity=2)
