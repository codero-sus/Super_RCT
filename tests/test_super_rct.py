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
VERSION = "0.3.0"
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
    def test_generated_tables_are_current(self):
        res = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "gen_commands.py"),
                              "--check"], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
