#!/usr/bin/env python3
"""Integration tests for Super_RCT.

Starts a mock Telegram Bot API server on localhost, runs the real
build/super_rct binary against it, and checks the replies it sends.
"""
import json
import os
import signal
import subprocess
import tempfile
import threading
import time
import unittest
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = os.environ.get("SUPER_RCT_BINARY") or os.path.join(ROOT, "build", "super_rct")
TOKEN = "123456789:TEST_token_for_mock_only"
ALLOWED_CHAT = 42
STRANGER_CHAT = 999


class QuietServer(ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        pass  # the bot may be killed mid-request; that is expected


class MockTelegram:
    """Serves queued updates once, then empty polls; records all calls."""

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
                elif method == "getUpdates":
                    with mock.lock:
                        mock.offsets.append(int(params.get("offset", 0)))
                        batch = mock.batches.pop(0) if mock.batches else []
                    if not batch:
                        time.sleep(0.2)
                    payload = {"ok": True, "result": batch}
                elif method == "sendMessage":
                    with mock.lock:
                        mock.sent.append((int(params["chat_id"]), params["text"]))
                    payload = {"ok": True, "result": {"message_id": 1}}
                else:
                    payload = {"ok": False, "description": "unknown method"}
                data = json.dumps(payload).encode()
                self.send_response(200)
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


class SuperRctTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.exists(BINARY):
            raise unittest.SkipTest("build/super_rct missing; run `make` first")

    def run_bot(self, batches, expect_replies, chat=ALLOWED_CHAT, timeout=15):
        """Runs the bot until `expect_replies` replies reach `chat`, then stops it."""
        self.tmp = tempfile.mkdtemp()
        self.ipfile = os.path.join(self.tmp, "ips.txt")
        with MockTelegram(batches) as mock:
            env = dict(os.environ,
                       SUPER_RCT_TOKEN=TOKEN,
                       SUPER_RCT_CHAT_IDS=f"{ALLOWED_CHAT}, 7",
                       SUPER_RCT_API=mock.url,
                       SUPER_RCT_IP_FILE=self.ipfile)
            proc = subprocess.Popen([BINARY], env=env, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True)
            deadline = time.time() + timeout
            while time.time() < deadline:
                if len(mock.replies_to(chat)) >= expect_replies:
                    break
                if proc.poll() is not None:
                    break
                time.sleep(0.05)
            time.sleep(0.3)  # allow any stray replies to land
            proc.send_signal(signal.SIGTERM)
            try:
                out, err = proc.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                out, err = proc.communicate()
            self.exit_code = proc.returncode
            self.stderr = err
            self.offsets = list(mock.offsets)
            self.replies = mock.replies_to(chat)
            self.stranger_replies = mock.replies_to(STRANGER_CHAT)
        return self.replies

    def test_commands_and_registry(self):
        batch1 = [
            make_update(100, ALLOWED_CHAT, "/help"),
            make_update(101, ALLOWED_CHAT, "/status"),
            make_update(102, ALLOWED_CHAT, "/ip enable 10.0.0.5"),
            make_update(103, ALLOWED_CHAT, "/ip disable 2001:db8::1"),
            make_update(104, ALLOWED_CHAT, "/ip list"),
            make_update(105, ALLOWED_CHAT, "/ip enable 999.1.1.1"),
            make_update(106, ALLOWED_CHAT, "/ip enable"),
            make_update(107, ALLOWED_CHAT, "/bogus"),
            make_update(108, ALLOWED_CHAT, "/ip enable \"quoted\\\\"),
        ]
        replies = self.run_bot([batch1], expect_replies=9)

        self.assertEqual(self.exit_code, 0, self.stderr)
        self.assertTrue(replies[0].startswith("Super_RCT 0.2.0 commands:"), replies[0])
        self.assertIn("Host:", replies[1])
        self.assertEqual(replies[2], "10.0.0.5 is now enabled.")
        self.assertEqual(replies[3], "2001:db8::1 is now disabled.")
        self.assertEqual(replies[4], "10.0.0.5 enabled\n2001:db8::1 disabled\n")
        self.assertEqual(replies[5], "Invalid IP address: 999.1.1.1")
        self.assertEqual(replies[6], "Usage: /ip enable <ip>")
        self.assertEqual(replies[7], "Unknown command. Send /help for the list.")
        self.assertIn("Invalid IP address", replies[8])

    def test_registry_persisted(self):
        self.run_bot([[make_update(1, ALLOWED_CHAT, "/ip enable 192.168.1.9"),
                       make_update(2, ALLOWED_CHAT, "/ip disable 192.168.1.9")]],
                     expect_replies=2)
        with open(self.ipfile) as f:
            self.assertEqual(f.read(), "disabled 192.168.1.9\n")

    def test_unauthorised_chat_gets_no_reply(self):
        self.run_bot([[make_update(1, STRANGER_CHAT, "/status"),
                       make_update(2, ALLOWED_CHAT, "/help")]],
                     expect_replies=1)
        self.assertEqual(self.stranger_replies, [])
        self.assertEqual(len(self.replies), 1)

    def test_offset_advances_past_last_update(self):
        self.run_bot([[make_update(500, ALLOWED_CHAT, "/help"),
                       make_update(501, ALLOWED_CHAT, "/help")]],
                     expect_replies=2)
        self.assertEqual(self.offsets[0], 0)
        self.assertIn(502, self.offsets)

    def test_bad_token_reports_error_without_crashing(self):
        # Bot that never gets a valid token: API returns ok=false on every poll.
        self.tmp = tempfile.mkdtemp()
        with MockTelegram([]) as mock:
            env = dict(os.environ, SUPER_RCT_TOKEN="111111111:WRONG_token_value",
                       SUPER_RCT_CHAT_IDS=str(ALLOWED_CHAT), SUPER_RCT_API=mock.url,
                       SUPER_RCT_IP_FILE=os.path.join(self.tmp, "ips.txt"))
            proc = subprocess.Popen([BINARY], env=env, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True)
            time.sleep(1.5)
            self.assertIsNone(proc.poll(), "bot should keep running on API errors")
            proc.send_signal(signal.SIGTERM)
            _, err = proc.communicate(timeout=10)
        self.assertIn("Unauthorized", err)

    def test_config_errors(self):
        for env_patch, needle in [
            ({"SUPER_RCT_TOKEN": "short"}, "SUPER_RCT_TOKEN"),
            ({"SUPER_RCT_TOKEN": TOKEN, "SUPER_RCT_CHAT_IDS": ""}, "SUPER_RCT_CHAT_IDS"),
            ({"SUPER_RCT_TOKEN": TOKEN, "SUPER_RCT_CHAT_IDS": "12x"}, "bad chat id"),
            ({"SUPER_RCT_TOKEN": TOKEN, "SUPER_RCT_CHAT_IDS": "1",
              "SUPER_RCT_API": "ftp://x"}, "SUPER_RCT_API"),
        ]:
            env = {k: v for k, v in os.environ.items() if not k.startswith("SUPER_RCT_")}
            env.update(env_patch)
            res = subprocess.run([BINARY], env=env, capture_output=True, text=True,
                                 timeout=10)
            self.assertEqual(res.returncode, 2, env_patch)
            self.assertIn(needle, res.stderr)

    def test_version_and_help_flags(self):
        res = subprocess.run([BINARY, "--version"], capture_output=True, text=True)
        self.assertEqual(res.stdout.strip(), "super_rct 0.2.0")
        res = subprocess.run([BINARY, "--help"], capture_output=True, text=True)
        self.assertIn("SUPER_RCT_TOKEN", res.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
