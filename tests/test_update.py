#!/usr/bin/env python3
"""Tests for tools/update.py. Uses a local bare repository as the remote; no network."""
import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import update  # noqa: E402

GIT_ENV = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.com",
               GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.com")


def git(cwd, *args):
    res = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True,
                         env=GIT_ENV, check=True)
    return res.stdout.strip()


def read(path):
    with open(path) as f:
        return f.read()


def write(path, text):
    with open(path, "w") as f:
        f.write(text)


class UpdateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.remote = os.path.join(self.tmp, "origin.git")
        self.work = os.path.join(self.tmp, "work")   # the other developer
        self.local = os.path.join(self.tmp, "local")  # the checkout being updated
        git(self.tmp, "init", "-q", "--bare", self.remote)
        git(self.tmp, "clone", "-q", self.remote, self.work)
        git(self.work, "checkout", "-q", "-b", "main")
        write(os.path.join(self.work, "a.txt"), "one\n")
        git(self.work, "add", "a.txt")
        git(self.work, "commit", "-q", "-m", "first")
        git(self.work, "push", "-q", "origin", "main")
        git(self.tmp, "clone", "-q", self.remote, self.local)
        git(self.local, "checkout", "-q", "main")

    def push_remote_change(self, text):
        write(os.path.join(self.work, "a.txt"), text)
        git(self.work, "commit", "-qam", "change")
        git(self.work, "push", "-q", "origin", "main")

    def run_update(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = update.main(list(argv), root=self.local)
        return code, out.getvalue(), err.getvalue()

    def test_check_reports_behind_and_changes_nothing(self):
        self.push_remote_change("two\n")
        code, out, _ = self.run_update()
        self.assertEqual(code, 0)
        self.assertIn("behind:   1 commit(s)", out)
        self.assertIn("M\ta.txt", out)
        self.assertEqual(read(os.path.join(self.local, "a.txt")), "one\n")

    def test_apply_fast_forwards(self):
        self.push_remote_change("two\n")
        code, out, _ = self.run_update("--apply")
        self.assertEqual(code, 0, out)
        self.assertIn("Updated to origin/main.", out)
        self.assertEqual(read(os.path.join(self.local, "a.txt")), "two\n")

    def test_apply_when_up_to_date(self):
        code, out, _ = self.run_update("--apply")
        self.assertEqual(code, 0)
        self.assertIn("Already up to date.", out)

    def test_apply_refuses_uncommitted_tracked_changes(self):
        self.push_remote_change("two\n")
        write(os.path.join(self.local, "a.txt"), "local edit\n")
        code, _, err = self.run_update("--apply")
        self.assertEqual(code, 1)
        self.assertIn("uncommitted changes", err)
        self.assertEqual(read(os.path.join(self.local, "a.txt")), "local edit\n")

    def test_apply_refuses_diverged_history(self):
        self.push_remote_change("two\n")
        write(os.path.join(self.local, "b.txt"), "local\n")
        git(self.local, "add", "b.txt")
        git(self.local, "commit", "-qm", "local work")
        code, _, err = self.run_update("--apply")
        self.assertEqual(code, 1)
        self.assertIn("local commits", err)

    def test_untracked_files_are_kept(self):
        self.push_remote_change("two\n")
        write(os.path.join(self.local, "secret.env"), "KEEP=1\n")
        code, _, _ = self.run_update("--apply")
        self.assertEqual(code, 0)
        self.assertEqual(read(os.path.join(self.local, "secret.env")), "KEEP=1\n")

    def test_no_upstream_is_explained(self):
        git(self.local, "branch", "--unset-upstream")
        code, _, err = self.run_update()
        self.assertEqual(code, 1)
        self.assertIn("no upstream", err)


if __name__ == "__main__":
    unittest.main()
