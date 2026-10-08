"""Monitoring and utility commands for the Python variant.

Mirrors src/monitor.h: same commands, same replies, same limits. Everything
here is reached only after /auth (see Bot.handle_message). The mixin reads and
writes these Bot attributes: chats, reply(), reply_long(), started, session_s,
auth_failures, cmds_handled, watch_paths, watch_max_h, registry, api_url,
version. Standard library only.
"""

import os
import re
import subprocess
import sys
import time

from commands_table import COMMANDS

CMD_TIMEOUT_S = 5                  # per external program, matches the C variant
METRIC_NAMES = ("mem", "disk", "temp", "load")
MEM, DISK, TEMP, LOAD = range(4)
HYSTERESIS = (5.0, 5.0, 5.0, 0.2)  # recovery margin per metric
MAX_DISKS = 48
MAX_REMINDERS = 32
MAX_REMINDERS_PER_CHAT = 10
MAX_REMINDER_TEXT = 200            # bytes
EVAL_INTERVAL_S = 60
REPORT_MIN_MIN = 5
REPORT_MAX_MIN = 1440
REAL_FS = frozenset({"ext2", "ext3", "ext4", "xfs", "btrfs", "f2fs", "vfat", "exfat",
                     "ntfs", "ntfs3", "zfs", "jfs"})

_INT_RE = re.compile(r"[+-]?[0-9]+")
_FLOAT_RE = re.compile(r"[+-]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?")
_REMIND_RE = re.compile(r"[ \t]*([+-]?[0-9]+)")
_MEMINFO_RE = re.compile(r"^(MemTotal|MemAvailable|SwapTotal|SwapFree):\s+([0-9]+)\s+kB",
                         re.MULTILINE)
_OCTAL_RE = re.compile(r"\\([0-7]{3})")


# ---------------------------------------------------------------------------
# Formatting helpers
# ---------------------------------------------------------------------------

def metric_text(m, v):
    if m in (MEM, DISK):
        return f"{v:.1f}%"
    if m == TEMP:
        return f"{v:.1f} C"
    return f"{v:.2f} per core"


def metric_range(m, v):
    if m == TEMP:
        return 20 <= v <= 120
    if m == LOAD:
        return 0.1 <= v <= 100
    return 1 <= v <= 100


def fmt_clock(t):
    return time.strftime("%H:%M", time.localtime(t))


def fmt_duration(secs):
    s = int(secs)
    return f"{s // 86400}d {(s % 86400) // 3600}h {(s % 3600) // 60}m"


def count_lines(text, needle=""):
    """Counts non-blank lines, or lines containing `needle` when it is not empty."""
    if needle == "":
        return sum(1 for line in text.split("\n") if line.strip(" \t\r"))
    return sum(1 for line in text.split("\n") if needle in line)


# ---------------------------------------------------------------------------
# Readers for /proc and /sys (None or empty when unavailable)
# ---------------------------------------------------------------------------

def _read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return None


def read_meminfo():
    text = _read("/proc/meminfo")
    if text is None:
        return None
    vals = {m.group(1): int(m.group(2)) for m in _MEMINFO_RE.finditer(text)}
    total = vals.get("MemTotal", 0)
    if total <= 0:
        return None
    avail = vals.get("MemAvailable", 0)
    stotal = vals.get("SwapTotal", 0)
    sfree = vals.get("SwapFree", 0)
    return {
        "mem_pct": 100.0 * (total - avail) / total,
        "swap_pct": 100.0 * (stotal - sfree) / stotal if stotal > 0 else 0.0,
        "mem_total_mb": total // 1024,
        "mem_used_mb": (total - avail) // 1024,
        "swap_total_mb": stotal // 1024,
        "swap_used_mb": (stotal - sfree) // 1024,
    }


def read_load():
    text = _read("/proc/loadavg")
    if text is None:
        return None
    parts = text.split()
    try:
        return tuple(float(x) for x in parts[:3]) if len(parts) >= 3 else None
    except ValueError:
        return None


def ncpus():
    return os.cpu_count() or 1


def read_temp_c():
    """Hottest thermal zone in degrees C, or None if no sensor is readable."""
    best = None
    for i in range(20):
        text = _read(f"/sys/class/thermal/thermal_zone{i}/temp")
        if text is None:
            continue
        try:
            c = int(text.split()[0]) / 1000.0
        except (ValueError, IndexError):
            continue
        if best is None or c > best:
            best = c
    return best


def read_cpu():
    """(busy, total) jiffies from the aggregate cpu line, or None."""
    text = _read("/proc/stat")
    if text is None:
        return None
    for line in text.split("\n"):
        if line.startswith("cpu "):
            fields = line.split()[1:]
            try:
                v = [int(x) for x in fields[:8]]
            except ValueError:
                return None
            if len(v) != 8:
                return None
            total = sum(v)
            return total - v[3] - v[4], total  # busy = total - idle - iowait
    return None


def cpu_busy_pct(seconds):
    first = read_cpu()
    if first is None:
        return None
    time.sleep(seconds)
    second = read_cpu()
    if second is None or second[1] <= first[1]:
        return None
    return 100.0 * (second[0] - first[0]) / (second[1] - first[1])


def net_bytes():
    """(rx, tx) byte totals across interfaces other than lo, or None."""
    text = _read("/proc/net/dev")
    if text is None:
        return None
    rx = tx = 0
    any_iface = False
    for line in text.split("\n"):
        if ":" not in line:
            continue
        name, _, rest = line.partition(":")
        if name.strip() == "lo":
            continue
        fields = rest.split()
        if len(fields) >= 9:
            try:
                rx += int(fields[0])
                tx += int(fields[8])
                any_iface = True
            except ValueError:
                pass
    return (rx, tx) if any_iface else None


def read_uptime():
    text = _read("/proc/uptime")
    if text is None:
        return None
    try:
        return float(text.split()[0])
    except (ValueError, IndexError):
        return None


def count_procs():
    try:
        return sum(1 for name in os.listdir("/proc") if re.fullmatch(r"[0-9]+", name))
    except OSError:
        return -1


def disk_scan():
    """[(mount, fstype, pct)] for each real mounted filesystem, like df."""
    text = _read("/proc/mounts")
    if text is None:
        return []
    out = []
    for line in text.split("\n"):
        parts = line.split()
        if len(parts) < 3 or parts[2] not in REAL_FS:
            continue
        mount = _OCTAL_RE.sub(lambda m: chr(int(m.group(1), 8)), parts[1])
        if any(mount == entry[0] for entry in out):
            continue
        try:
            sv = os.statvfs(mount)
        except OSError:
            continue
        if sv.f_blocks == 0:
            continue
        used = sv.f_blocks - sv.f_bfree
        avail = sv.f_bavail
        if used + avail <= 0:
            continue
        out.append((mount, parts[2], 100.0 * used / (used + avail)))
        if len(out) >= MAX_DISKS:
            break
    return out


def run_argv(argv):
    """Runs a fixed argv with no shell. Returns (status, output bytes).

    Status 127 means the program is missing, 124 means it timed out.
    """
    try:
        proc = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=CMD_TIMEOUT_S)
        return proc.returncode, proc.stdout
    except OSError:
        return 127, b""
    except subprocess.TimeoutExpired as err:
        return 124, err.stdout or b""


def _text(data):
    return data.decode("utf-8", "replace")


# ---------------------------------------------------------------------------
# The mixin
# ---------------------------------------------------------------------------

STATE_HEADER = ("# Super_RCT settings (saved by the bot; do not edit while it runs).\n"
                "# chat mem disk temp load report_minutes   (-1 = alert off, 0 = no report)\n")


def load_state(path, chats):
    """Applies saved alert limits and report intervals to `chats`.

    A missing file is normal on first run. Bad lines are skipped with a
    warning on stderr, so one damaged line cannot stop the bot starting.
    """
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except FileNotFoundError:
        return
    except OSError as err:
        print(f"warning: cannot read {path}: {err}", file=sys.stderr)
        return
    for line in lines:
        parts = line.split()
        if not parts or parts[0].startswith("#"):
            continue
        try:
            chat = int(parts[0])
            thr = [float(x) for x in parts[1:5]]
            minutes = int(parts[5])
            if len(parts) != 6:
                raise ValueError("wrong field count")
        except ValueError:
            print(f"warning: skipping bad line in {path}", file=sys.stderr)
            continue
        st = chats.get(chat)
        if st is None:
            continue  # chat no longer allowed in the config
        ok = all(t == -1.0 or (t >= 0 and metric_range(k, t)) for k, t in enumerate(thr))
        ok = ok and (minutes == 0 or REPORT_MIN_MIN <= minutes <= REPORT_MAX_MIN)
        if not ok:
            print(f"warning: skipping out-of-range settings for chat {chat} in {path}",
                  file=sys.stderr)
            continue
        st.thr = thr
        st.fired = [False] * 4
        st.report_every = minutes * 60


def save_state(path, chats):
    """Writes non-default settings atomically with mode 0600. Returns False on failure."""
    lines = [STATE_HEADER.rstrip("\n")]
    for chat, st in sorted(chats.items()):
        if all(t < 0 for t in st.thr) and st.report_every <= 0:
            continue
        fields = [str(chat)] + [repr(t) for t in st.thr] + [str(st.report_every // 60)]
        lines.append(" ".join(fields))
    tmp = path + ".tmp"
    try:
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        os.replace(tmp, path)
        return True
    except OSError:
        return False


def grade(value, warn, fail):
    """OK, WARN or FAIL for a reading against its warning and failure limits."""
    if value >= fail:
        return "FAIL"
    if value >= warn:
        return "WARN"
    return "OK"


def read_psi_some(name):
    """10-second average of 'some' pressure (percent) from /proc/pressure, or None."""
    try:
        with open(f"/proc/pressure/{name}", encoding="ascii") as f:
            for line in f:
                if line.startswith("some "):
                    for field in line.split():
                        if field.startswith("avg10="):
                            return float(field[len("avg10="):])
    except (OSError, ValueError):
        pass
    return None


class MonitorMixin:
    def init_monitor(self, cfg, version):
        self.version = version
        self.state_file = cfg["state_file"]
        self.api_url = cfg["api"]
        self.watch_paths = list(cfg["watch_paths"])
        self.watch_max_h = cfg["watch_max_h"]
        self.auth_failures = 0
        self.cmds_handled = 0
        self.reminders = [None] * MAX_REMINDERS  # slot -> dict or None
        self.next_rem_id = 1
        self.next_eval = 0

    # -- metrics and alerts --------------------------------------------------

    def collect_metrics(self):
        """Returns [value or None] indexed by MEM, DISK, TEMP, LOAD."""
        vals = [None] * 4
        mi = read_meminfo()
        if mi:
            vals[MEM] = mi["mem_pct"]
        disks = disk_scan()
        if disks:
            vals[DISK] = max(d[2] for d in disks)
        temp = read_temp_c()
        if temp is not None:
            vals[TEMP] = temp
        load = read_load()
        if load:
            vals[LOAD] = load[0] / ncpus()
        return vals

    def evaluate_alerts(self, vals):
        for c in self.chats.values():
            for k in range(4):
                if c.thr[k] < 0 or vals[k] is None:
                    continue
                if not c.fired[k] and vals[k] >= c.thr[k]:
                    c.fired[k] = True
                    self.reply(c.chat,
                               f"ALERT {METRIC_NAMES[k]} is {metric_text(k, vals[k])}, "
                               f"at or above the limit of {metric_text(k, c.thr[k])}")
                elif c.fired[k] and vals[k] < c.thr[k] - HYSTERESIS[k]:
                    c.fired[k] = False
                    self.reply(c.chat, f"RECOVERED {METRIC_NAMES[k]} is back to "
                                       f"{metric_text(k, vals[k])}")

    def build_health(self):
        lines = [f"Health (Super_RCT {self.version})"]

        mi = read_meminfo()
        if mi:
            lines.append(f"Memory: {mi['mem_pct']:.1f}% used "
                         f"({mi['mem_used_mb']}/{mi['mem_total_mb']} MB)")
            if mi["swap_total_mb"] > 0:
                lines.append(f"Swap: {mi['swap_pct']:.1f}% used "
                             f"({mi['swap_used_mb']}/{mi['swap_total_mb']} MB)")
            else:
                lines.append("Swap: none")
        else:
            lines.append("Memory: n/a")

        disks = disk_scan()
        if disks:
            worst = max(disks, key=lambda d: d[2])
            lines.append(f"Disk worst: {worst[2]:.1f}% on {worst[0]}")
        else:
            lines.append("Disk: n/a")

        temp = read_temp_c()
        lines.append(f"Temperature: {temp:.1f} C (hottest zone)" if temp is not None
                     else "Temperature: n/a")

        load = read_load()
        if load:
            lines.append(f"Load: {load[0]:.2f} {load[1]:.2f} {load[2]:.2f} "
                         f"({load[0] / ncpus():.2f} per core)")
        else:
            lines.append("Load: n/a")

        procs = count_procs()
        if procs >= 0:
            lines.append(f"Processes: {procs}")
        up = read_uptime()
        if up is not None:
            lines.append(f"Uptime: {fmt_duration(up)}")

        status, out = run_argv(["systemctl", "--failed", "--no-legend", "--plain"])
        lines.append(f"Failed units: {count_lines(_text(out))}" if status == 0
                     else "Failed units: n/a")
        status, out = run_argv(["apt", "list", "--upgradable"])
        lines.append(f"Updates available: {count_lines(_text(out), '[upgradable')}"
                     if status == 0 else "Updates available: n/a")
        lines.append("Reboot required: "
                     + ("yes" if os.path.exists("/var/run/reboot-required") else "no"))
        return "\n".join(lines) + "\n"

    def send_health(self, chat):
        self.reply_long(chat, self.build_health())

    def build_diagnose(self):
        """Checks the same readings as /health, and gives each one a verdict."""
        rows = []  # (level, text); "n/a" readings are left out
        mi = read_meminfo()
        if mi:
            rows.append((grade(mi["mem_pct"], 90, 97), f"Memory {mi['mem_pct']:.1f}% used"))
            if mi["swap_total_mb"] > 0:
                rows.append((grade(mi["swap_pct"], 50, 80),
                             f"Swap {mi['swap_pct']:.1f}% used"))
        psi = read_psi_some("memory")
        if psi is not None:
            rows.append((grade(psi, 10, 25), f"Memory pressure (10 s) {psi:.2f}%"))
        disks = disk_scan()
        if disks:
            worst = max(disks, key=lambda d: d[2])
            rows.append((grade(worst[2], 90, 98),
                         f"Disk {worst[2]:.1f}% used on {worst[0]}"))
        load = read_load()
        if load:
            per_core = load[0] / ncpus()
            rows.append((grade(per_core, 1.0, 2.0), f"Load {per_core:.2f} per core"))
        temp = read_temp_c()
        if temp is not None:
            rows.append((grade(temp, 80, 90), f"Temperature {temp:.1f} C (hottest zone)"))
        status, out = run_argv(["systemctl", "--failed", "--no-legend", "--plain"])
        if status == 0:
            failed = count_lines(_text(out), "")
            rows.append((grade(failed, 1, 3), f"Failed units: {failed}"))
        status, out = run_argv(["timedatectl", "show", "-p", "NTPSynchronized", "--value"])
        if status == 0:
            synced = _text(out).strip() == "yes"
            rows.append(("OK" if synced else "WARN",
                         "Clock is synchronised" if synced else "Clock is not synchronised"))
        if os.path.exists("/var/run/reboot-required"):
            rows.append(("WARN", "Reboot required"))
        status, out = run_argv(["apt", "list", "--upgradable"])
        if status == 0:
            updates = count_lines(_text(out), "[upgradable")
            rows.append(("INFO" if updates else "OK", f"Updates available: {updates}"))

        warnings = sum(1 for level, _ in rows if level == "WARN")
        failures = sum(1 for level, _ in rows if level == "FAIL")
        lines = [f"Diagnosis (Super_RCT {self.version})"]
        lines += [f"{level:<4} {text}" for level, text in rows]
        lines.append(f"Result: {failures} failure(s), {warnings} warning(s)")
        return "\n".join(lines) + "\n"

    def send_diagnose(self, chat):
        self.reply_long(chat, self.build_diagnose())

    def monitor_tick(self, now):
        """Called from the main loop: alerts, scheduled reports, reminders."""
        if now >= self.next_eval and any(
                c.thr[k] >= 0 for c in self.chats.values() for k in range(4)):
            self.next_eval = now + EVAL_INTERVAL_S
            self.evaluate_alerts(self.collect_metrics())

        for c in self.chats.values():
            if c.report_every > 0 and now >= c.report_next:
                c.report_next = now + c.report_every
                self.send_health(c.chat)

        for i, rem in enumerate(self.reminders):
            if rem and rem["due"] <= now:
                self.reminders[i] = None
                self.reply(rem["chat"], f"Reminder #{rem['id']}: {rem['text']}")

    # -- command handlers ----------------------------------------------------

    def cmd_mem(self, chat):
        mi = read_meminfo()
        if not mi:
            self.reply(chat, "Memory information is not available on this host.")
            return
        self.reply(chat, f"Memory: {mi['mem_pct']:.1f}% used\n"
                         f"Used: {mi['mem_used_mb']} MB of {mi['mem_total_mb']} MB")

    def cmd_swap(self, chat):
        mi = read_meminfo()
        if not mi:
            self.reply(chat, "Memory information is not available on this host.")
            return
        if mi["swap_total_mb"] > 0:
            self.reply(chat, f"Swap: {mi['swap_pct']:.1f}% used\n"
                             f"Used: {mi['swap_used_mb']} MB of {mi['swap_total_mb']} MB")
        else:
            self.reply(chat, "Swap: none configured")

    def cmd_disk(self, chat):
        disks = disk_scan()
        if not disks:
            self.reply(chat, "No real filesystems found.")
            return
        self.reply_long(chat, "".join(f"{m}  {p:.1f}%  ({fs})\n" for m, fs, p in disks))

    def cmd_load(self, chat):
        load = read_load()
        if not load:
            self.reply(chat, "Load information is not available on this host.")
            return
        self.reply(chat, f"Load: 1 min {load[0]:.2f}, 5 min {load[1]:.2f}, "
                         f"15 min {load[2]:.2f}\nCPUs: {ncpus()}\n"
                         f"Per CPU (1 min): {load[0] / ncpus():.2f}")

    def cmd_temp(self, chat):
        temp = read_temp_c()
        if temp is None:
            self.reply(chat, "No thermal sensors found on this host.")
        else:
            self.reply(chat, f"Hottest thermal zone: {temp:.1f} C")

    def cmd_cpu(self, chat):
        pct = cpu_busy_pct(1)
        if pct is None:
            self.reply(chat, "CPU statistics are not available on this host.")
        else:
            self.reply(chat, f"CPU busy: {pct:.1f}% (sampled over 1 s)")

    def cmd_net_rate(self, chat):
        first = net_bytes()
        if first is None:
            self.reply(chat, "Network statistics are not available on this host.")
            return
        time.sleep(1)
        second = net_bytes()
        if second is None:
            self.reply(chat, "Network statistics are not available on this host.")
            return
        self.reply(chat, "Network (excluding lo), 1 s sample:\n"
                         f"Receive: {(second[0] - first[0]) / 1024.0:.1f} KB/s\n"
                         f"Transmit: {(second[1] - first[1]) / 1024.0:.1f} KB/s")

    def cmd_procs(self, chat):
        n = count_procs()
        if n < 0:
            self.reply(chat, "Process list is not available on this host.")
        else:
            self.reply(chat, f"Running processes: {n}")

    def cmd_uptime_h(self, chat):
        up = read_uptime()
        if up is None:
            self.reply(chat, "Uptime is not available on this host.")
        else:
            self.reply(chat, f"Up {fmt_duration(up)}")

    def cmd_failed_units(self, chat):
        status, out = run_argv(["systemctl", "--failed", "--no-legend", "--plain"])
        if status != 0:
            self.reply(chat, "systemd is not available on this host.")
            return
        body = _text(out)
        self.reply_long(chat, f"Failed units: {count_lines(body)}\n" + body)

    def cmd_updates_count(self, chat):
        status, out = run_argv(["apt", "list", "--upgradable"])
        if status != 0:
            self.reply(chat, "Package updates are not available on this host (apt).")
        else:
            self.reply(chat, f"Packages with updates available: "
                             f"{count_lines(_text(out), '[upgradable')}")

    def cmd_reboot_required(self, chat):
        self.reply(chat, "Reboot required: "
                         + ("yes" if os.path.exists("/var/run/reboot-required") else "no"))

    def persist_settings(self, chat):
        if not save_state(self.state_file, self.chats):
            self.reply(chat, "Warning: settings could not be saved and will reset on restart.")

    def cmd_alert(self, chat, st, arg):
        parts = arg.split()
        sub = parts[0][:15] if parts else ""
        name = parts[1][:15] if len(parts) > 1 else ""
        val = parts[2][:31] if len(parts) > 2 else ""

        if sub in ("", "list"):
            lines = ["Alert limits (reply lists current settings):"]
            for k in range(4):
                if st.thr[k] < 0:
                    lines.append(f"{METRIC_NAMES[k]}: off")
                else:
                    lines.append(f"{METRIC_NAMES[k]}: {metric_text(k, st.thr[k])}")
            self.reply_long(chat, "\n".join(lines) + "\n")
            return

        if sub == "set":
            m = METRIC_NAMES.index(name) if name in METRIC_NAMES else -1
            if m < 0 or not val:
                self.reply(chat, "Usage: /alert set mem|disk|temp|load <value>")
                return
            if not _FLOAT_RE.fullmatch(val) or not metric_range(m, float(val)):
                self.reply(chat, f"Value for {name} is out of range.")
                return
            st.thr[m] = float(val)
            st.fired[m] = False
            self.persist_settings(chat)
            self.reply(chat, f"Alert for {name} set; you will be notified when it is reached.")
            return

        if sub == "off":
            m = METRIC_NAMES.index(name) if name in METRIC_NAMES else -1
            if m < 0:
                self.reply(chat, "Usage: /alert off mem|disk|temp|load")
                return
            st.thr[m] = -1.0
            st.fired[m] = False
            self.persist_settings(chat)
            self.reply(chat, f"Alert for {name} turned off.")
            return

        if sub == "check":
            vals = self.collect_metrics()
            lines = ["Current values:"]
            for k in range(4):
                if vals[k] is None:
                    lines.append(f"{METRIC_NAMES[k]}: n/a")
                    continue
                text = f"{METRIC_NAMES[k]}: {metric_text(k, vals[k])}"
                if st.thr[k] >= 0:
                    text += f" (limit {metric_text(k, st.thr[k])})"
                lines.append(text)
            self.reply_long(chat, "\n".join(lines) + "\n")
            return

        self.reply(chat, "Usage: /alert list | set <metric> <value> | off <metric> | check")

    def cmd_report(self, chat, st, arg, now):
        parts = arg.split()
        sub = parts[0][:15] if parts else ""
        val = parts[1][:15] if len(parts) > 1 else ""

        if sub in ("", "status"):
            if st.report_every <= 0:
                self.reply(chat, "Scheduled reports are off. Use /report on <minutes>.")
            else:
                self.reply(chat, f"Health reports every {st.report_every // 60} minutes; "
                                 f"next at {fmt_clock(st.report_next)}.")
            return
        if sub == "on":
            if not _INT_RE.fullmatch(val) or not REPORT_MIN_MIN <= int(val) <= REPORT_MAX_MIN:
                self.reply(chat, f"Usage: /report on <minutes>  "
                                 f"({REPORT_MIN_MIN}-{REPORT_MAX_MIN})")
                return
            mins = int(val)
            st.report_every = mins * 60
            st.report_next = now + st.report_every
            self.persist_settings(chat)
            self.reply(chat, f"Health reports on: every {mins} minutes.")
            return
        if sub == "off":
            st.report_every = 0
            self.persist_settings(chat)
            self.reply(chat, "Scheduled reports turned off.")
            return
        if sub == "now":
            self.send_health(chat)
            return
        self.reply(chat, "Usage: /report status | on <minutes> | off | now")

    def cmd_remind(self, chat, arg, now):
        usage = "Usage: /remind <minutes 1-10080> <text>"
        m = _REMIND_RE.match(arg)
        if not m or not 1 <= int(m.group(1)) <= 10080:
            self.reply(chat, usage)
            return
        mins = int(m.group(1))
        text = arg[m.end():].lstrip(" \t")
        if not text:
            self.reply(chat, usage)
            return
        if len(text.encode()) > MAX_REMINDER_TEXT:
            self.reply(chat, f"Reminder text is too long (max {MAX_REMINDER_TEXT} characters).")
            return
        mine = sum(1 for r in self.reminders if r and r["chat"] == chat)
        if mine >= MAX_REMINDERS_PER_CHAT or None not in self.reminders:
            self.reply(chat, f"You have too many reminders pending (max {MAX_REMINDERS_PER_CHAT}).")
            return
        slot = self.reminders.index(None)
        due = now + mins * 60
        self.reminders[slot] = {"id": self.next_rem_id, "chat": chat, "due": due, "text": text}
        self.next_rem_id += 1
        self.reply(chat, f"Reminder #{self.reminders[slot]['id']} set for {fmt_clock(due)}.")

    def cmd_timers(self, chat):
        lines = [f"#{r['id']} at {fmt_clock(r['due'])}: {r['text']}\n"
                 for r in self.reminders if r and r["chat"] == chat]
        if lines:
            self.reply_long(chat, "".join(lines))
        else:
            self.reply(chat, "No pending reminders.")

    def cmd_timer_cancel(self, chat, arg):
        if not _INT_RE.fullmatch(arg):
            self.reply(chat, "Usage: /timer_cancel <id>")
            return
        rid = int(arg)
        for i, r in enumerate(self.reminders):
            if r and r["chat"] == chat and r["id"] == rid:
                self.reminders[i] = None
                self.reply(chat, f"Reminder #{rid} cancelled.")
                return
        self.reply(chat, f"No pending reminder with id {rid}.")

    def watch_line(self, path, now, stale_only):
        try:
            st = os.stat(path)
        except OSError:
            return f"{path}  (missing)\n"
        age = (now - st.st_mtime) / 3600.0
        if stale_only and age <= self.watch_max_h:
            return None
        stale = "  STALE" if age > self.watch_max_h else ""
        return f"{path}  age {age:.1f} h  {st.st_size} bytes{stale}\n"

    def cmd_watch(self, chat, stale_only, now):
        if not self.watch_paths:
            self.reply(chat, "No watch paths configured (set SUPER_RCT_WATCH_PATHS).")
            return
        lines = [line for line in (self.watch_line(p, now, stale_only)
                                   for p in self.watch_paths) if line]
        if stale_only and not lines:
            text = (f"All {len(self.watch_paths)} watched paths are fresh "
                    f"(limit {self.watch_max_h} h).")
        else:
            text = "".join(lines)
        self.reply_long(chat, text)

    def cmd_notify(self, chat, arg):
        if not arg:
            self.reply(chat, "Usage: /notify <text>")
            return
        raw = arg.encode()[:MAX_REMINDER_TEXT].decode("utf-8", "ignore")
        clean = "".join(" " if ord(ch) < 0x20 else ch for ch in raw)
        status, _ = run_argv(["notify-send", "Super_RCT", clean])
        if status == 0:
            self.reply(chat, "Notification sent to the host.")
        elif status == 127:
            self.reply(chat, "notify-send is not installed on this host.")
        else:
            self.reply(chat, f"notify-send failed (exit {status}).")

    def cmd_botstats(self, chat, now):
        active = sum(1 for c in self.chats.values() if c.session_until > now)
        alerts = sum(1 for c in self.chats.values() if any(t >= 0 for t in c.thr))
        pending = sum(1 for r in self.reminders if r)
        self.reply(chat,
                   f"Uptime: {now - int(self.started)} s\n"
                   f"Commands handled: {self.cmds_handled}\n"
                   f"Failed logins: {self.auth_failures}\n"
                   f"Active sessions: {active} of {len(self.chats)}\n"
                   f"Chats with alerts: {alerts}\n"
                   f"Reminders pending: {pending}\n"
                   f"Watched paths: {len(self.watch_paths)}")

    def cmd_sessions(self, chat, now):
        lines = []
        for c in self.chats.values():
            if c.session_until > now:
                lines.append(f"chat {c.chat}: logged in, {c.session_until - now} s left\n")
            else:
                lines.append(f"chat {c.chat}: not logged in\n")
        self.reply_long(chat, "".join(lines))

    def cmd_logout_all(self, chat):
        for c in self.chats.values():
            c.session_until = 0
        self.reply(chat, "All login sessions ended. Send /auth <password> to log in again.")

    def cmd_selftest(self, chat):
        lines = ["Config: OK",
                 "/proc readable: " + ("PASS" if os.access("/proc/meminfo", os.R_OK) else "FAIL")]
        try:
            os.statvfs("/")
            lines.append("Disk statistics: PASS")
        except OSError:
            lines.append("Disk statistics: FAIL")
        folder = os.path.dirname(self.registry.path) or "."
        lines.append("IP registry directory writable: "
                     + ("PASS" if os.access(folder, os.W_OK) else "FAIL"))
        for prog in ("notify-send", "systemctl"):
            status, _ = run_argv([prog, "--version"])
            lines.append(f"{prog}: " + ("not installed" if status == 127 else "installed"))
        lines.append(f"Commands loaded: {len(COMMANDS)}")
        self.reply_long(chat, "\n".join(lines) + "\n")

    def cmd_config(self, chat):
        host = self.api_url.split("://", 1)[-1].split("/", 1)[0]
        alerts = sum(1 for c in self.chats.values() if any(t >= 0 for t in c.thr))
        self.reply_long(chat,
                        f"Allowed chats: {len(self.chats)}\n"
                        f"Session timeout: {self.session_s} s\n"
                        f"API host: {host}\n"
                        f"IP registry file: {self.registry.path}\n"
                        f"Watch paths: {len(self.watch_paths)} (max age {self.watch_max_h} h)\n"
                        f"Chats with alerts: {alerts}\n"
                        "Token and password: not shown\n")

    def cmd_about(self, chat, now):
        self.reply(chat, f"Super_RCT {self.version} (variant: Python)\n"
                         f"Uptime: {fmt_duration(now - int(self.started))}\n"
                         f"Commands: {len(COMMANDS)}")

    def cmd_echo(self, chat, arg):
        if not arg:
            self.reply(chat, "Usage: /echo <text>")
        else:
            self.reply(chat, arg)

    # -- dispatch ------------------------------------------------------------

    def monitor_dispatch(self, chat, st, cmd, arg, now):
        """Handles a monitoring command. Returns False if `cmd` is not one of ours."""
        routes = {
            "/health": lambda: self.send_health(chat),
            "/diagnose": lambda: self.send_diagnose(chat),
            "/mem": lambda: self.cmd_mem(chat),
            "/swap": lambda: self.cmd_swap(chat),
            "/disk": lambda: self.cmd_disk(chat),
            "/load": lambda: self.cmd_load(chat),
            "/temp": lambda: self.cmd_temp(chat),
            "/cpu": lambda: self.cmd_cpu(chat),
            "/net_rate": lambda: self.cmd_net_rate(chat),
            "/procs": lambda: self.cmd_procs(chat),
            "/uptime_h": lambda: self.cmd_uptime_h(chat),
            "/failed_units": lambda: self.cmd_failed_units(chat),
            "/updates_count": lambda: self.cmd_updates_count(chat),
            "/reboot_required": lambda: self.cmd_reboot_required(chat),
            "/alert": lambda: self.cmd_alert(chat, st, arg),
            "/report": lambda: self.cmd_report(chat, st, arg, now),
            "/remind": lambda: self.cmd_remind(chat, arg, now),
            "/timers": lambda: self.cmd_timers(chat),
            "/timer_cancel": lambda: self.cmd_timer_cancel(chat, arg),
            "/watch": lambda: self.cmd_watch(chat, False, now),
            "/watch_check": lambda: self.cmd_watch(chat, True, now),
            "/notify": lambda: self.cmd_notify(chat, arg),
            "/echo": lambda: self.cmd_echo(chat, arg),
            "/botstats": lambda: self.cmd_botstats(chat, now),
            "/sessions": lambda: self.cmd_sessions(chat, now),
            "/logout_all": lambda: self.cmd_logout_all(chat),
            "/about": lambda: self.cmd_about(chat, now),
            "/selftest": lambda: self.cmd_selftest(chat),
            "/config": lambda: self.cmd_config(chat),
        }
        handler = routes.get(cmd)
        if handler is None:
            return False
        handler()
        return True
