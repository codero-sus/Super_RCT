#!/usr/bin/env python3
"""Super_RCT GUI builder: a desktop window over tools/builder.py.

    python3 tools/gui_builder.py

Uses tkinter, which ships with Python on Windows and macOS. On Linux install it
with your package manager (for example `sudo apt install python3-tk`). The same
steps as the command-line builder run here, and the log shows every command.
Run `python3 tools/builder.py --help` for the command-line version.
"""

import os
import platform
import queue
import subprocess
import sys
import threading
from pathlib import Path

import builder
from builder import (DEFAULT_NAME, DEFAULT_OUT, TARGETS, BuildError, Plan, build_steps,
                     describe, execute, resolve_out, validate_name)

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    from tkinter.scrolledtext import ScrolledText
except ImportError:  # tkinter is missing; the logic below still works and is tested
    tk = None

ROOT = builder.ROOT
POLL_MS = 100


# ---- logic (no tkinter needed) -------------------------------------------

def make_plan(target, out_text, name, dry_run):
    """Checks the form values and returns a Plan. Raises BuildError with a message."""
    if target not in TARGETS:
        raise BuildError("choose what to build")
    if not out_text.strip():
        raise BuildError("choose an output folder")
    return Plan(target=target, out_dir=resolve_out(out_text.strip()),
                name=validate_name(name.strip()), dry_run=dry_run)


def preview_lines(plan):
    """The steps the plan would run, as text lines, without running anything."""
    try:
        steps = build_steps(plan)
    except BuildError as err:
        return [f"error: {err}"]
    lines = [f"Steps for {plan.target}:"]
    lines += [f"  - {describe(step)}" for step in steps]
    lines += [f"note: {note}" for note in plan.notes]
    return lines


def open_folder_argv(path, system=None):
    """Command that opens a folder in the file manager, or None when os.startfile is used."""
    system = system or platform.system()
    if system == "Windows":
        return None
    if system == "Darwin":
        return ["open", str(path)]
    return ["xdg-open", str(path)]


def run_build(plan, emit):
    """Runs the build and reports every line through `emit`. Returns 0 or 1, never raises."""
    try:
        return execute(plan, emit=emit)
    except BuildError as err:
        emit(f"error: {err}")
        return 1


def platform_note():
    if platform.system() == "Linux":
        return "Running on Linux. Installers are built for Linux."
    return (f"Running on {platform.system()}. Executables are built for this system; "
            "build on each system you need.")


# ---- window --------------------------------------------------------------

class BuilderWindow:
    def __init__(self, root):
        self.root = root
        self.queue = queue.Queue()
        self.busy = False
        self.last_out = None

        root.title("Super_RCT builder")
        root.minsize(640, 520)
        frame = ttk.Frame(root, padding=12)
        frame.pack(fill="both", expand=True)
        frame.columnconfigure(1, weight=1)

        ttk.Label(frame, text="What to build", font=("TkDefaultFont", 11, "bold")) \
            .grid(row=0, column=0, columnspan=3, sticky="w")
        self.target = tk.StringVar(value=next(iter(TARGETS)))
        for row, (key, label) in enumerate(TARGETS.items(), 1):
            ttk.Radiobutton(frame, text=f"{label}", value=key, variable=self.target) \
                .grid(row=row, column=0, columnspan=3, sticky="w", padx=(8, 0))

        base = len(TARGETS) + 1
        ttk.Label(frame, text="Output folder").grid(row=base, column=0, sticky="w", pady=(12, 0))
        self.out = tk.StringVar(value=DEFAULT_OUT)
        ttk.Entry(frame, textvariable=self.out).grid(row=base, column=1, sticky="ew",
                                                    pady=(12, 0), padx=6)
        ttk.Button(frame, text="Browse...", command=self._browse) \
            .grid(row=base, column=2, pady=(12, 0))

        ttk.Label(frame, text="File name").grid(row=base + 1, column=0, sticky="w", pady=6)
        self.name = tk.StringVar(value=DEFAULT_NAME)
        ttk.Entry(frame, textvariable=self.name).grid(row=base + 1, column=1, sticky="ew",
                                                     pady=6, padx=6)

        self.dry = tk.BooleanVar(value=False)
        ttk.Checkbutton(frame, text="Dry run (show the steps, build nothing)",
                        variable=self.dry).grid(row=base + 2, column=0, columnspan=3,
                                                sticky="w")

        buttons = ttk.Frame(frame)
        buttons.grid(row=base + 3, column=0, columnspan=3, sticky="ew", pady=10)
        self.preview_btn = ttk.Button(buttons, text="Show steps", command=self._show_steps)
        self.preview_btn.pack(side="left")
        self.build_btn = ttk.Button(buttons, text="Build", command=self._build)
        self.build_btn.pack(side="left", padx=8)
        self.open_btn = ttk.Button(buttons, text="Open output folder",
                                   command=self._open_output, state="disabled")
        self.open_btn.pack(side="left")

        self.log = ScrolledText(frame, height=14, state="disabled", wrap="word")
        self.log.grid(row=base + 4, column=0, columnspan=3, sticky="nsew")
        frame.rowconfigure(base + 4, weight=1)

        self.status = tk.StringVar(value=platform_note())
        ttk.Label(frame, textvariable=self.status, foreground="gray") \
            .grid(row=base + 5, column=0, columnspan=3, sticky="w", pady=(8, 0))

        root.protocol("WM_DELETE_WINDOW", self._on_close)
        root.after(POLL_MS, self._poll)

    # -- helpers
    def _write(self, text):
        self.log.configure(state="normal")
        self.log.insert("end", text + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _clear(self):
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")

    def _form_plan(self):
        return make_plan(self.target.get(), self.out.get(), self.name.get(),
                         self.dry.get())

    # -- actions
    def _browse(self):
        chosen = filedialog.askdirectory(initialdir=str(ROOT))
        if chosen:
            try:
                self.out.set(str(Path(chosen).relative_to(ROOT)))
            except ValueError:
                self.out.set(chosen)

    def _show_steps(self):
        try:
            plan = self._form_plan()
        except BuildError as err:
            messagebox.showerror("Check the form", str(err))
            return
        self._clear()
        for line in preview_lines(plan):
            self._write(line)

    def _build(self):
        if self.busy:
            return
        try:
            plan = self._form_plan()
        except BuildError as err:
            messagebox.showerror("Check the form", str(err))
            return
        self.busy = True
        self.build_btn.configure(state="disabled")
        self.preview_btn.configure(state="disabled")
        self.open_btn.configure(state="disabled")
        self._clear()
        self.status.set("Building...")
        self.last_out = plan.out_dir
        threading.Thread(target=self._worker, args=(plan,), daemon=True).start()

    def _worker(self, plan):
        code = run_build(plan, emit=self.queue.put)
        self.queue.put(("done", code))

    def _poll(self):
        while True:
            try:
                item = self.queue.get_nowait()
            except queue.Empty:
                break
            if isinstance(item, tuple):
                self._finish(item[1])
            else:
                self._write(item)
        self.root.after(POLL_MS, self._poll)

    def _finish(self, code):
        self.busy = False
        self.build_btn.configure(state="normal")
        self.preview_btn.configure(state="normal")
        if code == 0:
            self.status.set("Finished. The output is in the folder shown in the log.")
            self.open_btn.configure(state="normal")
        else:
            self.status.set("The build failed. See the log for the command that failed.")

    def _open_output(self):
        folder = self.last_out
        if not folder or not folder.exists():
            messagebox.showinfo("Open output folder", "The output folder does not exist yet.")
            return
        argv = open_folder_argv(folder)
        if argv is None:
            os.startfile(folder)  # Windows only
        else:
            try:
                subprocess.Popen(argv)
            except OSError as err:
                messagebox.showerror("Open output folder", str(err))

    def _on_close(self):
        if self.busy and not messagebox.askyesno(
                "Build running", "A build is still running. Close the window anyway?"):
            return
        self.root.destroy()


def main(argv=None):
    if tk is None:
        print("error: tkinter is not available for this Python.\n"
              "  Windows and macOS: reinstall Python from python.org (Tk is included).\n"
              "  Linux: install it, for example `sudo apt install python3-tk`.\n"
              "  Without a window, use: python3 tools/builder.py --help", file=sys.stderr)
        return 1
    root = tk.Tk()
    BuilderWindow(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
