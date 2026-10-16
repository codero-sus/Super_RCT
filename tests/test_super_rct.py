#!/usr/bin/env python3
"""Integration tests for Super_RCT.

The same suite runs against both implementations:
  * build/super_rct          (C variant, build with `make`)
  * src/super_rct.py         (Python variant)

Each test starts a mock Telegram Bot API on localhost, runs the bot against
it, and checks the replies the bot sends.
"""
import json
import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "src"))
from commands_table import COMMANDS  # noqa: E402  (generated from src/commands.def)
C_BINARY = os.environ.get("SUPER_RCT_BINARY") or os.path.join(ROOT, "build", "super_rct")
PY_SCRIPT = os.path.join(ROOT, "src", "super_rct.py")

TOKEN = "123456789:TEST_token_for_mock_only"
PASSWORD = "correct horse battery"
ALLOWED_CHAT = 42
STRANGER_CHAT = 999
VERSION = "0.5.0"
AUTH = f"/auth {PASSWORD}"


class QuietServer(ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        pass  # the bot may be killed mid-request; that is expected


class MockTelegram:
    """Serves queued batches once, then empty polls; records every call.

    A float item in `batches` pauses the mock for that many seconds and
    returns an empty batch (used to test session expiry).
    """

    def __init__(self, batches):
        self.batches = list(batches)
        self.offsets = []
        self.sent = []  # (chat_id, text)
        self.lock = threading.Lock()
        mock = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                self._handle()

            def do_POST(self):
                self._handle()

            def _handle(self):
                length = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(length).decode() if length else ""
                params = dict(urllib.parse.parse_qsl(body))
                params.update(dict(urllib.parse.parse_qsl(
                    urllib.parse.urlparse(self.path).query)))
                method = self.path.split("?")[0].rsplit("/", 1)[-1]

                if self.path.split("/")[1] != f"bot{TOKEN}":
                    payload = {"ok": False, "error_code": 401,
                               "description": "Unauthorized"}
                    code = 401
                elif method == "getUpdates":
                    with mock.lock:
                        mock.offsets.append(int(params.get("offset", 0)))
                        item = mock.batches.pop(0) if mock.batches else []
                    if isinstance(item, float):
                        time.sleep(item)
                        item = []
                    elif not item:
                        time.sleep(0.2)
                    payload = {"ok": True, "result": item}
                    code = 200
                elif method == "sendMessage":
                    with mock.lock:
                        mock.sent.append((int(params["chat_id"]), params["text"]))
                    payload = {"ok": True, "result": {"message_id": 1}}
                    code = 200
                else:
                    payload = {"ok": False, "description": "unknown method"}
                    code = 404

                data = json.dumps(payload).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.server = QuietServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.server.shutdown()
        self.server.server_close()

    def replies_to(self, chat):
        with self.lock:
            return [t for c, t in self.sent if c == chat]


DISCORD_TOKEN = "DISCORD_TEST_TOKEN_abcdefghijklmnopqrstuvwxyz0123"
DISCORD_CHANNEL = 900001


class MockDiscord:
    """Serves channel messages like the Discord REST API; records posted messages."""

    def __init__(self, channels):
        self.messages = {c: list(m) for c, m in channels.items()}
        self.sent = []  # (channel, content)
        self.lock = threading.Lock()
        mock = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def _send(self, code, payload):
                data = json.dumps(payload).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def _authorised(self):
                if (self.headers.get("Authorization") == f"Bot {DISCORD_TOKEN}"
                        and self.headers.get("User-Agent")):
                    return True
                self._send(401, {"message": "401: Unauthorized", "code": 0})
                return False

            def do_GET(self):
                if not self._authorised():
                    return
                url = urllib.parse.urlparse(self.path)
                channel = int(url.path.split("/")[-2])
                query = dict(urllib.parse.parse_qsl(url.query))
                limit = int(query.get("limit", 50))
                with mock.lock:
                    msgs = list(mock.messages.get(channel, []))
                if "after" in query:
                    msgs = sorted((m for m in msgs if int(m["id"]) > int(query["after"])),
                                  key=lambda m: int(m["id"]))[:limit]
                    self._send(200, list(reversed(msgs)))  # Discord: newest first
                else:
                    msgs = sorted(msgs, key=lambda m: int(m["id"]), reverse=True)[:limit]
                    self._send(200, msgs)

            def do_POST(self):
                if not self._authorised():
                    return
                length = int(self.headers.get("Content-Length") or 0)
                body = json.loads(self.rfile.read(length).decode() or "{}")
                channel = int(self.path.split("/")[-2])
                with mock.lock:
                    mock.sent.append((channel, body.get("content", "")))
                self._send(200, {"id": "999", "content": body.get("content", "")})

        self.server = QuietServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def add(self, channel, message):
        with self.lock:
            self.messages.setdefault(channel, []).append(message)

    def sent_texts(self):
        with self.lock:
            return [t for _c, t in self.sent]

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.server.shutdown()
        self.server.server_close()


def make_update(update_id, chat_id, text):
    # Telegram puts reply_to_message (which has its own "text") before the
    # real message text. The parser must read the top-level text only.
    return {
        "update_id": update_id,
        "message": {
            "message_id": update_id,
            "from": {"id": chat_id, "is_bot": False, "first_name": "Tester"},
            "chat": {"id": chat_id, "first_name": "Tester", "type": "private"},
            "date": 1700000000,
            "reply_to_message": {"message_id": 1, "text": "/status (quoted)"},
            "text": text,
        },
    }


def base_env(**overrides):
    env = {k: v for k, v in os.environ.items() if not k.startswith("SUPER_RCT_")}
    env.update({
        "SUPER_RCT_TOKEN": TOKEN,
        "SUPER_RCT_CHAT_IDS": "42, 7",
        "SUPER_RCT_PASSWORD": PASSWORD,
        # Never route the local mock through an HTTP proxy.
        "NO_PROXY": "127.0.0.1,localhost",
        "no_proxy": "127.0.0.1,localhost",
    })
    for key, value in overrides.items():
        if value is None:
            env.pop(key, None)
        else:
            env[key] = value
    return env


class BotCases:
    """Shared test cases. Subclasses set `command` to the bot to run."""

    command = None  # list of argv items

    @classmethod
    def setUpClass(cls):
        if cls.command is None:
            raise unittest.SkipTest("base class")
        if cls.command[0] == C_BINARY and not os.path.exists(C_BINARY):
            raise unittest.SkipTest("build/super_rct missing; run `make` first")

    def run_bot(self, batches, expect_replies, chat=ALLOWED_CHAT, timeout=20,
                settle=0.3, **env):
        """Runs the bot until `expect_replies` replies reach `chat`, then stops it."""
        self.tmp = tempfile.mkdtemp()
        self.ipfile = os.path.join(self.tmp, "ips.txt")
        with MockTelegram(batches) as mock:
            env.setdefault("SUPER_RCT_API", mock.url)
            env.setdefault("SUPER_RCT_IP_FILE", self.ipfile)
            env.setdefault("SUPER_RCT_STATE_FILE", os.path.join(self.tmp, "state.txt"))
            proc = subprocess.Popen(self.command, env=base_env(**env),
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    text=True)
            deadline = time.time() + timeout
            while time.time() < deadline:
                if len(mock.replies_to(chat)) >= expect_replies or proc.poll() is not None:
                    break
                time.sleep(0.05)
            time.sleep(settle)  # let any further replies land
            proc.send_signal(signal.SIGTERM)
            try:
                _, err = proc.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                _, err = proc.communicate()
            self.exit_code = proc.returncode
            self.stderr = err
            self.offsets = list(mock.offsets)
            self.replies = mock.replies_to(chat)
            self.stranger_replies = mock.replies_to(STRANGER_CHAT)
        return self.replies

    # -- auth flow ----------------------------------------------------------

    def test_commands_require_auth(self):
        replies = self.run_bot([[
            make_update(100, ALLOWED_CHAT, "/status"),
            make_update(101, ALLOWED_CHAT, "/auth wrong-password"),
            make_update(102, ALLOWED_CHAT, AUTH),
            make_update(103, ALLOWED_CHAT, "/status"),
        ]], expect_replies=4)
        self.assertEqual(replies[0], "Not authenticated. Send /auth <password> first.")
        self.assertEqual(replies[1], "Wrong password (1/3 failed attempts).")
        self.assertTrue(replies[2].startswith("Authenticated. Session expires after 3600 s"),
                        replies[2])
        self.assertIn("Host:", replies[3])

    def test_wrong_password_lockout(self):
        replies = self.run_bot([[
            make_update(1, ALLOWED_CHAT, "/auth nope-nope-1"),
            make_update(2, ALLOWED_CHAT, "/auth nope-nope-2"),
            make_update(3, ALLOWED_CHAT, "/auth nope-nope-3"),
            make_update(4, ALLOWED_CHAT, AUTH),  # correct, but locked out
        ]], expect_replies=4)
        self.assertEqual(replies[0], "Wrong password (1/3 failed attempts).")
        self.assertEqual(replies[1], "Wrong password (2/3 failed attempts).")
        self.assertEqual(replies[2],
                         "Wrong password. Too many failed attempts; locked for 300 s.")
        self.assertIn("Too many failed attempts. Try again in", replies[3])

    def test_logout_ends_session(self):
        replies = self.run_bot([[
            make_update(1, ALLOWED_CHAT, AUTH),
            make_update(2, ALLOWED_CHAT, "/logout"),
            make_update(3, ALLOWED_CHAT, "/status"),
        ]], expect_replies=3)
        self.assertTrue(replies[0].startswith("Authenticated."))
        self.assertEqual(replies[1], "Logged out.")
        self.assertEqual(replies[2], "Not authenticated. Send /auth <password> first.")

    def test_session_expires_after_idle_timeout(self):
        replies = self.run_bot([
            [make_update(1, ALLOWED_CHAT, "/auth " + PASSWORD)],
            6.0,  # idle longer than SUPER_RCT_SESSION_SECONDS=5
            [make_update(2, ALLOWED_CHAT, "/status")],
        ], expect_replies=2, SUPER_RCT_SESSION_SECONDS="5")
        self.assertTrue(replies[0].startswith("Authenticated."))
        self.assertEqual(replies[1], "Not authenticated. Send /auth <password> first.")

    def test_password_never_logged(self):
        self.run_bot([[make_update(1, ALLOWED_CHAT, AUTH),
                       make_update(2, ALLOWED_CHAT, "/auth bad-password-here")]],
                     expect_replies=2)
        self.assertNotIn(PASSWORD, self.stderr)
        self.assertNotIn("bad-password-here", self.stderr)

    def test_unauthorised_chat_gets_no_reply_and_cannot_log_in(self):
        self.run_bot([[make_update(1, STRANGER_CHAT, AUTH),
                       make_update(2, STRANGER_CHAT, "/status"),
                       make_update(3, ALLOWED_CHAT, "/help")]],
                     expect_replies=1)
        self.assertEqual(self.stranger_replies, [])
        self.assertEqual(len(self.replies), 1)

    # -- table commands (files and fixed programs) ---------------------------

    def test_table_commands_require_auth(self):
        replies = self.run_bot([[make_update(1, ALLOWED_CHAT, "/meminfo")]],
                               expect_replies=1)
        self.assertEqual(replies, ["Not authenticated. Send /auth <password> first."])

    def test_file_and_program_outputs(self):
        replies = self.run_bot([[
            make_update(1, ALLOWED_CHAT, AUTH),
            make_update(2, ALLOWED_CHAT, "/meminfo"),
            make_update(3, ALLOWED_CHAT, "/uname"),
            make_update(4, ALLOWED_CHAT, "/nosuchcmd"),
            make_update(5, ALLOWED_CHAT, "/sys_nonexistent_knob"),
        ]], expect_replies=5)
        self.assertTrue(replies[1].startswith("/meminfo\n"), replies[1][:80])
        self.assertIn("MemTotal:", replies[1])
        self.assertTrue(replies[2].startswith("/uname\n"), replies[2][:80])
        self.assertIn("Linux", replies[2])
        self.assertEqual(replies[3], "Unknown command. Send /help for the list.")
        self.assertEqual(replies[4], "Unknown command. Send /help for the list.")

    def test_commands_listing_matches_generated_table(self):
        replies = self.run_bot([[make_update(1, ALLOWED_CHAT, AUTH),
                                 make_update(2, ALLOWED_CHAT, "/commands")]],
                               expect_replies=2, settle=1.0)
        text = "".join(replies[1:])
        self.assertTrue(text.startswith(f"Commands ({len(COMMANDS)}):\n"), text[:80])
        for name, _kind, _target, desc in COMMANDS:
            self.assertIn(f"/{name} - {desc}\n", text)
        real = [c for c in COMMANDS if c[1] != "b"]
        self.assertGreaterEqual(len(real), 99)
        for chunk in replies[1:]:
            self.assertLessEqual(len(chunk), 4096)

    # -- commands and registry ----------------------------------------------

    def test_commands_and_registry(self):
        replies = self.run_bot([[
            make_update(100, ALLOWED_CHAT, "/status"),
            make_update(101, ALLOWED_CHAT, "/auth wrong-password"),
            make_update(102, ALLOWED_CHAT, AUTH),
            make_update(103, ALLOWED_CHAT, "/help"),
            make_update(104, ALLOWED_CHAT, "/status"),
            make_update(105, ALLOWED_CHAT, "/ip enable 10.0.0.5"),
            make_update(106, ALLOWED_CHAT, "/ip disable 2001:db8::1"),
            make_update(107, ALLOWED_CHAT, "/ip list"),
            make_update(108, ALLOWED_CHAT, "/ip enable 999.1.1.1"),
            make_update(109, ALLOWED_CHAT, "/ip enable"),
            make_update(110, ALLOWED_CHAT, "/bogus"),
        ]], expect_replies=11)

        self.assertEqual(self.exit_code, 0, self.stderr)
        self.assertTrue(replies[3].startswith(f"Super_RCT {VERSION} commands:"), replies[3])
        self.assertIn("Host:", replies[4])
        self.assertEqual(replies[5], "10.0.0.5 is now enabled.")
        self.assertEqual(replies[6], "2001:db8::1 is now disabled.")
        self.assertEqual(replies[7], "10.0.0.5 enabled\n2001:db8::1 disabled\n")
        self.assertEqual(replies[8], "Invalid IP address: 999.1.1.1")
        self.assertEqual(replies[9], "Usage: /ip enable <ip>")
        self.assertEqual(replies[10], "Unknown command. Send /help for the list.")

    def test_registry_persisted(self):
        self.run_bot([[make_update(1, ALLOWED_CHAT, AUTH),
                       make_update(2, ALLOWED_CHAT, "/ip enable 192.168.1.9"),
                       make_update(3, ALLOWED_CHAT, "/ip disable 192.168.1.9")]],
                     expect_replies=3)
        with open(self.ipfile) as f:
            self.assertEqual(f.read(), "disabled 192.168.1.9\n")

    def test_offset_advances_past_last_update(self):
        self.run_bot([[make_update(500, ALLOWED_CHAT, AUTH),
                       make_update(501, ALLOWED_CHAT, "/help")]],
                     expect_replies=2)
        self.assertEqual(self.offsets[0], 0)
        self.assertIn(502, self.offsets)

    # -- resilience ---------------------------------------------------------

    def test_api_error_is_logged_and_backed_off(self):
        self.tmp = tempfile.mkdtemp()
        with MockTelegram([]) as mock:
            proc = subprocess.Popen(
                self.command,
                env=base_env(SUPER_RCT_TOKEN="111111111:WRONG_token_value",
                             SUPER_RCT_API=mock.url,
                             SUPER_RCT_IP_FILE=os.path.join(self.tmp, "ips.txt")),
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            time.sleep(1.5)
            self.assertIsNone(proc.poll(), "bot should keep running on API errors")
            proc.send_signal(signal.SIGTERM)
            _, err = proc.communicate(timeout=10)
        self.assertIn("Unauthorized", err)
        # Backs off between errors instead of hammering the API.
        self.assertLessEqual(len(mock.offsets), 2, mock.offsets)

    # -- Discord transport ---------------------------------------------------

    def test_discord_commands(self):
        tmp = tempfile.mkdtemp()
        history = [{"id": "100", "content": "/auth stale-history",
                    "author": {"bot": False}}]
        dc = MockDiscord({DISCORD_CHANNEL: history})
        with MockTelegram([]) as tg, dc:
            env = base_env(SUPER_RCT_API=tg.url,
                           SUPER_RCT_IP_FILE=os.path.join(tmp, "ips.txt"),
                           SUPER_RCT_DISCORD_TOKEN=DISCORD_TOKEN,
                           SUPER_RCT_DISCORD_CHANNEL_IDS=str(DISCORD_CHANNEL),
                           SUPER_RCT_DISCORD_API=dc.url + "/api/v10")
            proc = subprocess.Popen(self.command, env=env, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True)
            time.sleep(1.5)  # the first poll only records the newest message
            dc.add(DISCORD_CHANNEL, {"id": "101", "content": AUTH, "author": {"bot": False}})
            dc.add(DISCORD_CHANNEL, {"id": "102", "content": "/echo hi",
                                     "author": {"bot": False}})
            dc.add(DISCORD_CHANNEL, {"id": "103", "content": "/echo from-bot",
                                     "author": {"bot": True}})
            deadline = time.time() + 25
            while time.time() < deadline and len(dc.sent_texts()) < 2:
                time.sleep(0.1)
            time.sleep(0.5)
            proc.send_signal(signal.SIGTERM)
            try:
                _, err = proc.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                _, err = proc.communicate()
        texts = dc.sent_texts()
        self.assertEqual(len(texts), 2, (texts, err))
        self.assertTrue(texts[0].startswith("Authenticated."), texts[0])
        self.assertEqual(texts[1], "hi")
        self.assertEqual(tg.sent, [])  # Discord replies never go to Telegram

    NEW_SAMPLE = ["sys_vm_dirty_bytes", "uname_release", "uname_kernel_name", "time_date_rfc",
                  "proc_ps_cpu_top", "disk_df_types", "net_ip_brief_addr", "systemd_boot_time",
                  "systemd_failed", "log_dmesg_err"]

    def assert_command_replies(self, replies, names):
        self.assertEqual(len(replies), len(names), replies)
        for reply, name in zip(replies, names):
            with self.subTest(command=name):
                self.assertTrue(reply.startswith(f"/{name}\n"), reply[:80])
                self.assertNotIn("Unknown command", reply)

    def test_new_commands_telegram(self):
        names = self.NEW_SAMPLE
        batch = [make_update(1, ALLOWED_CHAT, AUTH)]
        batch += [make_update(i + 2, ALLOWED_CHAT, "/" + n) for i, n in enumerate(names)]
        replies = self.run_bot([batch], expect_replies=len(names) + 1, timeout=60, settle=0.5)
        self.assertEqual(self.exit_code, 0, self.stderr)
        self.assert_command_replies(replies[1:], names)

    def test_new_commands_discord(self):
        names = self.NEW_SAMPLE[:6]
        tmp = tempfile.mkdtemp()
        dc = MockDiscord({DISCORD_CHANNEL: []})
        with MockTelegram([]) as tg, dc:
            env = base_env(SUPER_RCT_API=tg.url,
                           SUPER_RCT_IP_FILE=os.path.join(tmp, "ips.txt"),
                           SUPER_RCT_STATE_FILE=os.path.join(tmp, "state.txt"),
                           SUPER_RCT_DISCORD_TOKEN=DISCORD_TOKEN,
                           SUPER_RCT_DISCORD_CHANNEL_IDS=str(DISCORD_CHANNEL),
                           SUPER_RCT_DISCORD_API=dc.url + "/api/v10")
            proc = subprocess.Popen(self.command, env=env, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True)
            time.sleep(1.5)  # the first poll only records the newest message
            dc.add(DISCORD_CHANNEL, {"id": "101", "content": AUTH, "author": {"bot": False}})
            for i, name in enumerate(names):
                dc.add(DISCORD_CHANNEL, {"id": str(102 + i), "content": "/" + name,
                                         "author": {"bot": False}})
            deadline = time.time() + 60
            while time.time() < deadline and len(dc.sent_texts()) < len(names) + 1:
                time.sleep(0.1)
            time.sleep(0.5)
            proc.send_signal(signal.SIGTERM)
            try:
                _, err = proc.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                _, err = proc.communicate()
        texts = dc.sent_texts()
        self.assertTrue(texts[0].startswith("Authenticated."), (texts, err))
        # Long replies are split into several Discord messages, so look for each
        # command's header in order instead of expecting one message per command.
        pos = 1
        for name in names:
            with self.subTest(command=name):
                while pos < len(texts) and not texts[pos].startswith(f"/{name}\n"):
                    pos += 1
                self.assertLess(pos, len(texts), (name, texts[1:3]))
                self.assertNotIn("Unknown command", texts[pos])
            pos += 1

    def test_discord_config_errors(self):
        cases = [
            ({"SUPER_RCT_DISCORD_TOKEN": "short"}, "SUPER_RCT_DISCORD_TOKEN"),
            ({"SUPER_RCT_DISCORD_TOKEN": DISCORD_TOKEN}, "SUPER_RCT_DISCORD_CHANNEL_IDS"),
            ({"SUPER_RCT_DISCORD_TOKEN": DISCORD_TOKEN,
              "SUPER_RCT_DISCORD_CHANNEL_IDS": "42"}, "appears in both"),
            ({"SUPER_RCT_DISCORD_TOKEN": DISCORD_TOKEN,
              "SUPER_RCT_DISCORD_CHANNEL_IDS": "900001",
              "SUPER_RCT_DISCORD_API": "ftp://example.com"}, "SUPER_RCT_DISCORD_API"),
        ]
        for overrides, needle in cases:
            with self.subTest(overrides=overrides):
                res = subprocess.run(self.command, env=base_env(**overrides),
                                     capture_output=True, text=True, timeout=10)
                self.assertEqual(res.returncode, 2, res.stderr)
                self.assertIn(needle, res.stderr)

    # -- alert cooldown and quiet hours --------------------------------------

    def test_alert_cooldown_and_quiet_hours_survive_restart(self):
        state = os.path.join(tempfile.mkdtemp(), "state.txt")
        first = [[make_update(1, ALLOWED_CHAT, AUTH),
                  make_update(2, ALLOWED_CHAT, "/alert set mem 80"),
                  make_update(3, ALLOWED_CHAT, "/alert cooldown 30"),
                  make_update(4, ALLOWED_CHAT, "/alert quiet 22 7")]]
        self.run_bot(first, expect_replies=5, SUPER_RCT_STATE_FILE=state)
        second = [[make_update(1, ALLOWED_CHAT, AUTH),
                   make_update(2, ALLOWED_CHAT, "/alert list")]]
        replies = self.run_bot(second, expect_replies=2, SUPER_RCT_STATE_FILE=state)
        text = "\n".join(replies)
        self.assertIn("Cooldown between alerts: 30 min", text)
        self.assertIn("Quiet hours (server time): 22:00 to 07:00", text)

    def test_alert_cooldown_and_quiet_hours_are_validated(self):
        replies = self.run_bot([[
            make_update(1, ALLOWED_CHAT, AUTH),
            make_update(2, ALLOWED_CHAT, "/alert cooldown 5000"),
            make_update(3, ALLOWED_CHAT, "/alert quiet 5 5"),
            make_update(4, ALLOWED_CHAT, "/alert quiet 25 3"),
            make_update(5, ALLOWED_CHAT, "/alert quiet off"),
        ]], expect_replies=5)
        self.assertEqual(self.exit_code, 0, self.stderr)
        self.assertIn("Usage: /alert cooldown", replies[1])
        self.assertIn("Usage: /alert quiet", replies[2])
        self.assertIn("Usage: /alert quiet", replies[3])
        self.assertEqual(replies[4], "Quiet hours turned off.")

    def test_six_field_state_lines_still_load(self):
        """State files written before cooldowns existed (6 fields) keep working."""
        state = os.path.join(tempfile.mkdtemp(), "state.txt")
        with open(state, "w", encoding="utf-8") as f:
            f.write(f"{ALLOWED_CHAT} 85.0 -1 -1 -1 0\n")
        replies = self.run_bot([[make_update(1, ALLOWED_CHAT, AUTH),
                                 make_update(2, ALLOWED_CHAT, "/alert list")]],
                               expect_replies=2, SUPER_RCT_STATE_FILE=state)
        text = "\n".join(replies)
        self.assertIn("mem: 85", text)
        self.assertIn("Cooldown between alerts: off", text)
        self.assertIn("Quiet hours: off", text)

    # -- /run (shell command, behind /auth) ------------------------------------

    def test_run_executes_shell_command(self):
        replies = self.run_bot([[
            make_update(1, ALLOWED_CHAT, "/run echo before-auth"),
            make_update(2, ALLOWED_CHAT, AUTH),
            make_update(3, ALLOWED_CHAT, "/run echo hi-there; echo second-line; exit 3"),
        ]], expect_replies=3)
        self.assertEqual(self.exit_code, 0, self.stderr)
        self.assertEqual(len(replies), 3, replies)
        self.assertFalse(replies[0].startswith("/run\n"), replies[0])  # refused before /auth
        run_reply = replies[2]
        self.assertTrue(run_reply.startswith("/run\n"), run_reply)
        self.assertIn("hi-there\nsecond-line", run_reply)
        self.assertIn("(exit 3)", run_reply)

    def test_run_can_be_turned_off(self):
        replies = self.run_bot([[
            make_update(1, ALLOWED_CHAT, AUTH),
            make_update(2, ALLOWED_CHAT, "/run echo should-not-run"),
        ]], expect_replies=2, SUPER_RCT_SHELL="0")
        self.assertEqual(self.exit_code, 0, self.stderr)
        self.assertIn("disabled", replies[1])
        self.assertNotIn("should-not-run", replies[1])

    def test_shell_setting_must_be_0_or_1(self):
        res = subprocess.run(self.command, env=base_env(SUPER_RCT_SHELL="maybe"),
                             capture_output=True, text=True, timeout=10)
        self.assertEqual(res.returncode, 2, res.stderr)
        self.assertIn("SUPER_RCT_SHELL", res.stderr)

    # -- monitoring and utilities (all behind /auth) -------------------------

    def test_settings_survive_restart(self):
        state = os.path.join(tempfile.mkdtemp(), "state.txt")
        first = [[make_update(1, ALLOWED_CHAT, AUTH),
                  make_update(2, ALLOWED_CHAT, "/alert set mem 91"),
                  make_update(3, ALLOWED_CHAT, "/alert set load 3.5"),
                  make_update(4, ALLOWED_CHAT, "/report on 30")]]
        self.run_bot(first, expect_replies=4, SUPER_RCT_STATE_FILE=state)
        self.assertEqual(os.stat(state).st_mode & 0o777, 0o600)

        second = [[make_update(1, ALLOWED_CHAT, AUTH),
                   make_update(2, ALLOWED_CHAT, "/alert list"),
                   make_update(3, ALLOWED_CHAT, "/report status")]]
        replies = self.run_bot(second, expect_replies=3, SUPER_RCT_STATE_FILE=state)
        text = "\n".join(replies)
        self.assertIn("mem: 91", text)
        self.assertIn("load: 3.5", text)
        self.assertIn("Health reports every 30 minutes", text)

    def test_damaged_state_line_is_skipped(self):
        state = os.path.join(tempfile.mkdtemp(), "state.txt")
        with open(state, "w", encoding="utf-8") as f:
            f.write("# header\n"
                    "42 garbage here\n"
                    "42 -1 -1 -1 -1 0 extra\n"
                    "42 95 -1 -1 -1 0\n"
                    "7 50 -1 -1 -1 0\n")  # chat 7 is allowed, values are fine
        replies = self.run_bot([[make_update(1, ALLOWED_CHAT, AUTH),
                                 make_update(2, ALLOWED_CHAT, "/alert list")]],
                               expect_replies=2, SUPER_RCT_STATE_FILE=state)
        self.assertIn("mem: 95", "\n".join(replies))
        self.assertIn("warning", self.stderr)

    def test_monitoring_requires_auth(self):
        replies = self.run_bot([[make_update(1, ALLOWED_CHAT, "/health")]], expect_replies=1)
        self.assertEqual(replies, ["Not authenticated. Send /auth <password> first."])

    def test_monitoring_commands(self):
        cmds = [
            (AUTH, None),
            ("/auth wrong-password", None),
            ("/botstats", "Failed logins: 1"),
            ("/health", f"Health (Super_RCT {VERSION})"),
            ("/diagnose", r"re:^Diagnosis \(Super_RCT \d+\.\d+\.\d+\)\n"
                         r"(?:(?:OK|WARN|FAIL|INFO) +.*\n)*"
                         r"Result: \d+ failure\(s\), \d+ warning\(s\)\n"),
            ("/mem", "Memory"),
            ("/load", "Load:"),
            ("/uptime_h", "Up "),
            ("/procs", "Running processes:"),
            ("/reboot_required", "Reboot required: "),
            ("/cpu", "CPU "),
            ("/about", f"Super_RCT {VERSION} (variant: "),
            ("/echo hello  world", "hello  world"),
            ("/echo", "Usage: /echo <text>"),
            ("/remind 0 nope", "Usage: /remind <minutes 1-10080> <text>"),
            ("/remind 5 check the disk", r"re:Reminder #1 set for \d\d:\d\d\."),
            ("/timers", r"re:#1 at \d\d:\d\d: check the disk"),
            ("/timer_cancel 1", "Reminder #1 cancelled."),
            ("/timer_cancel 99", "No pending reminder with id 99."),
            ("/report status", "Scheduled reports are off. Use /report on <minutes>."),
            ("/report on 2", "Usage: /report on <minutes>  (5-1440)"),
            ("/report on 5", "Health reports on: every 5 minutes."),
            ("/report off", "Scheduled reports turned off."),
            ("/alert list", "Alert limits (reply lists current settings):\n"
                            "mem: off\ndisk: off\ntemp: off\nload: off\n"
                            "Cooldown between alerts: off\nQuiet hours: off\n"),
            ("/alert set mem 200", "Value for mem is out of range."),
            ("/alert set mem 99", "Alert for mem set; you will be notified when it is reached."),
            ("/alert off mem", "Alert for mem turned off."),
            ("/alert nope", "Usage: /alert list | set <metric> <value> | off <metric> | check | "
                            "cooldown <minutes> | quiet <start> <end> | quiet off"),
            ("/watch", "No watch paths configured (set SUPER_RCT_WATCH_PATHS)."),
            ("/sessions", "chat 42: logged in"),
            ("/config", "Allowed chats: 2"),
            ("/selftest", "Config: OK"),
            ("/logout_all", "All login sessions ended. Send /auth <password> to log in again."),
            ("/mem", "Not authenticated. Send /auth <password> first."),
        ]
        batch = [make_update(i + 1, ALLOWED_CHAT, text) for i, (text, _) in enumerate(cmds)]
        replies = self.run_bot([batch], expect_replies=len(cmds), timeout=40, settle=0.5)

        self.assertEqual(self.exit_code, 0, self.stderr)
        self.assertEqual(len(replies), len(cmds), replies)
        for reply, (text, expected) in zip(replies, cmds):
            if expected is None:
                continue
            with self.subTest(command=text):
                if expected.startswith("re:"):
                    self.assertRegex(reply, expected[3:])
                elif expected in ("Memory", "Load:", "Up ", "Running processes:",
                                  "Reboot required: ", "CPU ", "Config: OK"):
                    self.assertTrue(reply.startswith(expected) or expected in reply, reply[:80])
                elif expected.startswith("Health ("):
                    self.assertTrue(reply.startswith(expected), reply[:80])
                elif expected.startswith(("Super_RCT ", "chat ", "Allowed chats", "Failed logins")):
                    self.assertIn(expected, reply)
                else:
                    self.assertEqual(reply, expected)
        # Secrets never leave the host in /config.
        self.assertNotIn(TOKEN, replies[cmds.index(("/config", "Allowed chats: 2"))])
        self.assertNotIn(PASSWORD, replies[cmds.index(("/config", "Allowed chats: 2"))])

    def test_watch_paths(self):
        tmp = tempfile.mkdtemp()
        path = os.path.join(tmp, "backup.log")
        with open(path, "w") as f:
            f.write("ok\n")
        replies = self.run_bot([[
            make_update(1, ALLOWED_CHAT, AUTH),
            make_update(2, ALLOWED_CHAT, "/watch"),
            make_update(3, ALLOWED_CHAT, "/watch_check"),
        ]], expect_replies=3, SUPER_RCT_WATCH_PATHS=f"{path},/nonexistent_super_rct/x")
        self.assertIn(f"{path}  age ", replies[1])
        self.assertIn("/nonexistent_super_rct/x  (missing)", replies[1])
        self.assertIn("/nonexistent_super_rct/x  (missing)", replies[2])
        self.assertNotIn("are fresh", replies[2])

        replies = self.run_bot([[
            make_update(1, ALLOWED_CHAT, AUTH),
            make_update(2, ALLOWED_CHAT, "/watch_check"),
        ]], expect_replies=2, SUPER_RCT_WATCH_PATHS=path)
        self.assertEqual(replies[1], "All 1 watched paths are fresh (limit 24 h).")

    def test_alert_check_shows_values(self):
        replies = self.run_bot([[
            make_update(1, ALLOWED_CHAT, AUTH),
            make_update(2, ALLOWED_CHAT, "/alert set disk 98"),
            make_update(3, ALLOWED_CHAT, "/alert check"),
        ]], expect_replies=3)
        self.assertEqual(replies[1], "Alert for disk set; you will be notified when it is reached.")
        self.assertTrue(replies[2].startswith("Current values:\n"), replies[2])
        self.assertIn("disk: ", replies[2])
        self.assertIn("(limit 98.0%)", replies[2])

    def test_config_errors(self):
        cases = [
            ({"SUPER_RCT_TOKEN": "short"}, "SUPER_RCT_TOKEN"),
            ({"SUPER_RCT_TOKEN": None}, "SUPER_RCT_TOKEN"),
            ({"SUPER_RCT_PASSWORD": None}, "SUPER_RCT_PASSWORD"),
            ({"SUPER_RCT_PASSWORD": "short"}, "SUPER_RCT_PASSWORD"),
            ({"SUPER_RCT_CHAT_IDS": ""}, "SUPER_RCT_CHAT_IDS"),
            ({"SUPER_RCT_CHAT_IDS": "12x"}, "bad chat id"),
            ({"SUPER_RCT_CHAT_IDS": "1,,2"}, "bad chat id"),
            ({"SUPER_RCT_SESSION_SECONDS": "2"}, "SUPER_RCT_SESSION_SECONDS"),
            ({"SUPER_RCT_SESSION_SECONDS": "soon"}, "SUPER_RCT_SESSION_SECONDS"),
            ({"SUPER_RCT_API": "ftp://x"}, "SUPER_RCT_API"),
            ({"SUPER_RCT_WATCH_PATHS": "relative/x"}, "SUPER_RCT_WATCH_PATHS"),
            ({"SUPER_RCT_WATCH_MAX_AGE_HOURS": "0"}, "SUPER_RCT_WATCH_MAX_AGE_HOURS"),
        ]
        for overrides, needle in cases:
            with self.subTest(overrides=overrides):
                res = subprocess.run(self.command, env=base_env(**overrides),
                                     capture_output=True, text=True, timeout=10)
                self.assertEqual(res.returncode, 2, res.stderr)
                self.assertIn(needle, res.stderr)

    def test_version_and_help_flags(self):
        res = subprocess.run(self.command + ["--version"], capture_output=True, text=True)
        self.assertEqual(res.stdout.strip(), f"super_rct {VERSION}")
        res = subprocess.run(self.command + ["--help"], capture_output=True, text=True)
        self.assertIn("SUPER_RCT_PASSWORD", res.stdout)


class CBotTests(BotCases, unittest.TestCase):
    command = [C_BINARY]


class PythonBotTests(BotCases, unittest.TestCase):
    command = [sys.executable, PY_SCRIPT]



class GeneratedTableTests(unittest.TestCase):
    def test_every_file_and_program_command_gets_a_reply(self):
        """Runs every f and c entry through the Python bot in-process: each must answer."""
        sys.path.insert(0, os.path.join(ROOT, "src"))
        import super_rct  # noqa: E402

        tmp = tempfile.mkdtemp()
        env = base_env(SUPER_RCT_IP_FILE=os.path.join(tmp, "ips.txt"),
                       SUPER_RCT_STATE_FILE=os.path.join(tmp, "state.txt"))
        bot = super_rct.Bot(super_rct.load_config(env))
        replies = []
        bot.reply = lambda chat, text: replies.append(text)
        runnable = [c for c in COMMANDS if c[1] in ("f", "c")]
        for name, kind, target, _desc in runnable:
            bot.run_table(ALLOWED_CHAT, name, kind, target)
        self.assertEqual(len(replies), len(runnable))
        for reply, (name, _kind, _target, _desc) in zip(replies, runnable):
            with self.subTest(command=name):
                self.assertTrue(reply.startswith(f"/{name}\n"), reply[:80])
                self.assertTrue(reply.split("\n", 1)[1].strip(), "empty reply")

    def test_run_shell_kills_background_children_on_timeout(self):
        """A background child that keeps the output pipe open must not hang /run."""
        import monitor  # noqa: E402  (src is on sys.path)

        started = time.monotonic()
        status, out = monitor.run_shell("echo started; sleep 30 & sleep 30", 1)
        self.assertEqual(status, 124)
        self.assertIn(b"started", out)
        self.assertLess(time.monotonic() - started, 10)

    def test_alert_cooldown_quiet_hours_and_recovery(self):
        """Alert logic on the Python side: cooldown, quiet hours, recovery duration."""
        import monitor  # noqa: E402  (src is on sys.path)
        import super_rct  # noqa: E402

        tmp = tempfile.mkdtemp()
        env = base_env(SUPER_RCT_IP_FILE=os.path.join(tmp, "ips.txt"),
                       SUPER_RCT_STATE_FILE=os.path.join(tmp, "state.txt"))
        bot = super_rct.Bot(super_rct.load_config(env))
        sent = []
        bot.reply = lambda chat, text: sent.append(text)
        st = bot.chats[ALLOWED_CHAT]
        st.thr[monitor.MEM] = 80.0
        st.cooldown_min = 10
        t0 = time.mktime((2026, 1, 5, 12, 0, 0, 0, 0, -1))  # local noon, outside quiet hours
        high, low = [90.0, None, None, None], [50.0, None, None, None]

        bot.evaluate_alerts(high, t0)                   # ALERT
        bot.evaluate_alerts(low, t0 + 60)               # RECOVERED after 1 minute
        bot.evaluate_alerts(high, t0 + 120)             # inside cooldown: silent
        bot.evaluate_alerts(low, t0 + 180)              # nothing was announced: silent
        bot.evaluate_alerts(high, t0 + 900)             # cooldown over: ALERT again
        self.assertEqual(len(sent), 3, sent)
        self.assertTrue(sent[0].startswith("ALERT mem is"), sent[0])
        self.assertIn("after 0d 0h 1m", sent[1])
        self.assertTrue(sent[2].startswith("ALERT mem is"), sent[2])

        # Quiet hours: 22:00 to 07:00 server time, no messages at 23:00.
        st.cooldown_min = 0
        st.quiet_start, st.quiet_end = 22, 7
        st.fired[monitor.MEM] = False
        st.notified[monitor.MEM] = False
        before = len(sent)
        night = time.mktime((2026, 1, 5, 23, 0, 0, 0, 0, -1))
        bot.evaluate_alerts(high, night)
        bot.evaluate_alerts(low, night + 60)
        self.assertEqual(len(sent), before, sent)

    def test_table_has_at_least_570_entries(self):
        # 270 entries before this change, plus at least 300 new ones.
        self.assertGreaterEqual(len(COMMANDS), 570)

    def test_generated_tables_are_current(self):
        res = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "gen_commands.py"),
                              "--check"], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
