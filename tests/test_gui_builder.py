#!/usr/bin/env python3
"""Tests for tools/gui_builder.py. The window itself runs only where tkinter and a
display are available; everything else is tested without a window."""

import os
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import builder  # noqa: E402
import gui_builder  # noqa: E402


class FormTests(unittest.TestCase):
    def test_valid_form_becomes_plan(self):
        plan = gui_builder.make_plan("c", " build/out ", " tool ", True)
        self.assertEqual(plan.target, "c")
        self.assertEqual(plan.out_dir, ROOT / "build" / "out")
        self.assertEqual(plan.name, "tool")
        self.assertTrue(plan.dry_run)

    def test_bad_values_are_refused(self):
        cases = [
            ("", "x", "y", "choose what to build"),
            ("c", "  ", "y", "choose an output folder"),
            ("c", "x", "bad name", "bad name"),
        ]
        for target, out, name, needle in cases:
            with self.subTest(target=target, out=out, name=name):
                with self.assertRaises(builder.BuildError) as ctx:
                    gui_builder.make_plan(target, out, name, False)
                self.assertIn(needle, str(ctx.exception))


class PreviewTests(unittest.TestCase):
    def test_preview_lists_steps_and_runs_nothing(self):
        plan = builder.Plan(target="c", out_dir=Path("/tmp/none"), name="t")
        lines = gui_builder.preview_lines(plan)
        self.assertTrue(lines[0].startswith("Steps for c"))
        self.assertTrue(any("make release" in line for line in lines))
        self.assertFalse(os.path.exists("/tmp/none"))

    def test_preview_reports_missing_nuitka(self):
        plan = builder.Plan(target="py-nuitka", out_dir=Path("/tmp/none"), name="t")
        with mock.patch.object(builder.importlib.util, "find_spec", return_value=None):
            lines = gui_builder.preview_lines(plan)
        self.assertEqual(len(lines), 1)
        self.assertIn("Nuitka is not installed", lines[0])


class OpenFolderTests(unittest.TestCase):
    def test_command_per_system(self):
        self.assertEqual(gui_builder.open_folder_argv("/x", "Darwin"), ["open", "/x"])
        self.assertEqual(gui_builder.open_folder_argv("/x", "Linux"), ["xdg-open", "/x"])
        self.assertIsNone(gui_builder.open_folder_argv("/x", "Windows"))


class RunTests(unittest.TestCase):
    def test_run_command_streams_every_line(self):
        lines = []
        code = builder.run_command([sys.executable, "-c", "print('a'); print('b')"],
                                   str(ROOT), lines.append)
        self.assertEqual(code, 0)
        self.assertEqual(lines, ["a", "b"])

    def test_run_command_returns_failure_code(self):
        lines = []
        code = builder.run_command([sys.executable, "-c", "import sys; sys.exit(3)"],
                                   str(ROOT), lines.append)
        self.assertEqual(code, 3)

    def test_run_build_dry_run_emits_steps(self):
        lines = []
        plan = builder.Plan(target="c", out_dir=Path("/tmp/none"), name="t", dry_run=True)
        self.assertEqual(gui_builder.run_build(plan, lines.append), 0)
        self.assertTrue(lines[0].startswith("Dry run"))
        self.assertFalse(os.path.exists("/tmp/none"))

    def test_run_build_reports_bad_plan_without_raising(self):
        lines = []
        plan = builder.Plan(target="bogus", out_dir=Path("/tmp/none"), name="t")
        self.assertEqual(gui_builder.run_build(plan, lines.append), 1)
        self.assertIn("unknown target", lines[-1])

    def test_real_c_build_through_gui_logic(self):
        out = Path(os.environ.get("TMPDIR", "/tmp")) / f"srct-gui-{os.getpid()}"
        lines = []
        plan = builder.Plan(target="c", out_dir=out, name="gui_test")
        try:
            self.assertEqual(gui_builder.run_build(plan, lines.append), 0, lines)
            self.assertTrue((out / "gui_test").exists())
        finally:
            for item in out.glob("*"):
                item.unlink()
            if out.exists():
                out.rmdir()


class WindowTests(unittest.TestCase):
    def test_main_without_tkinter_explains_and_fails(self):
        with mock.patch.object(gui_builder, "tk", None), \
                mock.patch("sys.stderr") as err:
            code = gui_builder.main([])
        self.assertEqual(code, 1)
        printed = "".join(call.args[0] for call in err.write.call_args_list)
        self.assertIn("tkinter is not available", printed)

    @unittest.skipIf(gui_builder.tk is None, "tkinter is not installed")
    def test_window_builds_and_closes(self):
        root = gui_builder.tk.Tk()
        try:
            root.withdraw()
            window = gui_builder.BuilderWindow(root)
            window._show_steps()
            self.assertIn("Steps for", window.log.get("1.0", "end"))
        except gui_builder.tk.TclError:
            self.skipTest("no display available")
        finally:
            root.destroy()


if __name__ == "__main__":
    unittest.main(verbosity=2)
