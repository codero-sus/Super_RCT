#!/usr/bin/env python3
"""Super_RCT - a small Telegram remote-control bot (Python variant).

Behaviour mirrors src/super_rct.c: same commands, same replies, same
configuration variables. Standard library only (Python 3.8+).

Configuration (environment variables)
  SUPER_RCT_TOKEN            Telegram bot token (required)
  SUPER_RCT_CHAT_IDS         Comma-separated allowed chat IDs (required)
  SUPER_RCT_PASSWORD         Login password, 8-128 bytes (required)
  SUPER_RCT_SESSION_SECONDS  Idle timeout for a login (default 3600)
  SUPER_RCT_IP_FILE          IP registry file (default super_rct_ips.txt)
  SUPER_RCT_API              API base URL (default https://api.telegram.org)
"""

import hmac
import ipaddress
import json
import os
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

from commands_table import COMMANDS

VERSION = "0.3.0"
DEFAULT_API = "https://api.telegram.org"
DEFAULT_IP_FILE = "super_rct_ips.txt"
DEFAULT_SESSION_S = 3600
MIN_SESSION_S = 5
MAX_SESSION_S = 86400
MIN_PASSWORD = 8
MAX_PASSWORD = 128
MAX_CHATS = 32
MAX_IPS = 256
POLL_TIMEOUT_S = 25
HTTP_TIMEOUT_S = 40
SEND_TIMEOUT_S = 20
RETRY_DELAY_S = 5
MAX_FAILS = 3
LOCKOUT_S = 300
CMD_TIMEOUT_S = 5
READ_MAX = 65536
OUT_MAX = 3500  # body cap, well under Telegram's 4096 characters
CHUNK_MAX = 3500


class ConfigError(Exception):
    pass


class Stop(BaseException):
    """Raised by the signal handler to unwind a blocking network call."""


def log(msg):
    stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
    print(f"[{stamp}] {msg}", file=sys.stderr, flush=True)


def usage_text():
    return (
        f"Super_RCT {VERSION} - Telegram remote-control bot\n\n"
        "Usage: super_rct.py [--help] [--version]\n\n"
        "Environment:\n"
        "  SUPER_RCT_TOKEN            bot token (required)\n"
        "  SUPER_RCT_CHAT_IDS         allowed chat IDs, comma-separated (required)\n"
        "  SUPER_RCT_PASSWORD         login password, 8-128 bytes (required)\n"
        f"  SUPER_RCT_SESSION_SECONDS  idle timeout for a login (default {DEFAULT_SESSION_S})\n"
        f"  SUPER_RCT_IP_FILE          IP registry file (default {DEFAULT_IP_FILE})\n"
        f"  SUPER_RCT_API              API base URL (default {DEFAULT_API})\n"
    )


HELP_TEXT = (
    f"Super_RCT {VERSION} commands:\n"
    "/auth <password> - log in (required before other commands)\n"
    "/logout - end your session\n"
    "/status - host status\n"
    "/ip list - show IP registry\n"
    "/ip enable <ip> - mark an IP as enabled\n"
    "/ip disable <ip> - mark an IP as disabled\n"
    "/commands - list all commands\n"
    "/help - this message\n"
    "Note: the IP registry is informational. No firewall rules are changed."
)


def valid_token(token):
    if not 10 <= len(token) < 256:
        return False
    return all(c.isascii() and (c.isalnum() or c in ":_-") for c in token)


def parse_int(text):
    """Accepts an optionally signed integer with surrounding spaces, else None."""
    try:
        return int(text.strip())
    except ValueError:
        return None


def load_config(env):
    token = env.get("SUPER_RCT_TOKEN", "")
    if not valid_token(token):
        raise ConfigError("SUPER_RCT_TOKEN is missing or malformed")

    password = env.get("SUPER_RCT_PASSWORD", "")
    if not MIN_PASSWORD <= len(password.encode()) <= MAX_PASSWORD:
        raise ConfigError(f"SUPER_RCT_PASSWORD is required ({MIN_PASSWORD}-{MAX_PASSWORD} bytes)")

    session_s = DEFAULT_SESSION_S
    raw_session = env.get("SUPER_RCT_SESSION_SECONDS", "")
    if raw_session:
        value = parse_int(raw_session)
        if value is None or not MIN_SESSION_S <= value <= MAX_SESSION_S:
            raise ConfigError(
                f"SUPER_RCT_SESSION_SECONDS must be {MIN_SESSION_S}-{MAX_SESSION_S}")
        session_s = value

    raw_chats = env.get("SUPER_RCT_CHAT_IDS", "")
    if not raw_chats.strip():
        raise ConfigError("SUPER_RCT_CHAT_IDS is required (comma-separated)")
    chats = []
    for part in raw_chats.split(","):
        value = parse_int(part)
        if value is None or len(chats) >= MAX_CHATS:
            raise ConfigError("bad chat id list in SUPER_RCT_CHAT_IDS")
        chats.append(value)

    api = env.get("SUPER_RCT_API", "") or DEFAULT_API
    if not (api.startswith("https://") or api.startswith("http://")):
        raise ConfigError("SUPER_RCT_API must start with https:// or http://")

    ip_file = env.get("SUPER_RCT_IP_FILE", "") or DEFAULT_IP_FILE

    return {
        "token": token,
        "password": password,
        "session_s": session_s,
        "chats": chats,
        "api": api.rstrip("/"),
        "ip_file": ip_file,
    }


class Telegram:
    """Minimal Bot API client. Telegram sends JSON error bodies with HTTP 4xx."""

    def __init__(self, api, token):
        self.base = f"{api}/bot{token}"

    def call(self, method, params, timeout):
        data = urllib.parse.urlencode(params).encode()
        request = urllib.request.Request(f"{self.base}/{method}", data=data)
        try:
            with urllib.request.urlopen(request, timeout=timeout) as resp:
                body = resp.read()
        except urllib.error.HTTPError as err:
            body = err.read()
        return json.loads(body.decode("utf-8", "replace"))


class ChatState:
    __slots__ = ("chat", "session_until", "fails", "locked_until")

    def __init__(self, chat):
        self.chat = chat
        self.session_until = 0  # logged in while now < session_until
        self.fails = 0          # consecutive wrong passwords
        self.locked_until = 0   # refuses /auth while now < locked_until


class IpRegistry:
    """IP address -> enabled flag, persisted as 'enabled|disabled <ip>' lines."""

    def __init__(self, path):
        self.path = path
        self.entries = {}
        self._load()

    def _load(self):
        try:
            with open(self.path, encoding="utf-8") as f:
                lines = f.readlines()
        except OSError:
            return  # no registry yet
        for line in lines:
            parts = line.split()
            if len(parts) == 2 and parts[0] in ("enabled", "disabled") \
                    and len(self.entries) < MAX_IPS:
                self.entries[parts[1]] = parts[0] == "enabled"

    def save(self):
        tmp = f"{self.path}.tmp"
        try:
            with open(tmp, "w", encoding="utf-8") as f:
                for ip, enabled in self.entries.items():
                    f.write(f"{'enabled' if enabled else 'disabled'} {ip}\n")
            os.replace(tmp, self.path)
            return True
        except OSError:
            return False

    @staticmethod
    def normalise(text):
        """Returns the canonical IPv4/IPv6 form, or None if invalid."""
        if "%" in text:
            return None  # no scope IDs
        try:
            return str(ipaddress.ip_address(text))
        except ValueError:
            return None


def utf8_cut(data, max_len):
    """Byte index to cut `data` at, never inside a multi-byte character."""
    if len(data) <= max_len:
        return len(data)
    cut = max_len
    while cut > 0 and (data[cut] & 0xC0) == 0x80:
        cut -= 1
    return cut


def chunk_lines(text, limit=CHUNK_MAX):
    """Splits text into chunks of whole lines, each at most `limit` bytes."""
    chunks, current, current_len = [], [], 0
    for line in text.splitlines(keepends=True):
        size = len(line.encode())
        if current and current_len + size > limit:
            chunks.append("".join(current))
            current, current_len = [], 0
        current.append(line)
        current_len += size
    if current:
        chunks.append("".join(current))
    return chunks


class Bot:
    def __init__(self, cfg):
        self.tg = Telegram(cfg["api"], cfg["token"])
        self.password = cfg["password"].encode()
        self.session_s = cfg["session_s"]
        self.chats = {c: ChatState(c) for c in cfg["chats"]}
        self.registry = IpRegistry(cfg["ip_file"])
        self.offset = 0
        self.started = time.time()

    # -- transport -----------------------------------------------------------

    def reply(self, chat, text):
        try:
            resp = self.tg.call("sendMessage", {"chat_id": chat, "text": text},
                                SEND_TIMEOUT_S)
        except (OSError, ValueError) as err:
            log(f"sendMessage to chat {chat} failed: {err}")
            return
        if not resp.get("ok"):
            log(f"sendMessage to chat {chat} failed: {resp.get('description', '')}")

    # -- commands ------------------------------------------------------------

    def cmd_auth(self, st, arg, now):
        if st.locked_until > now:
            self.reply(st.chat, f"Too many failed attempts. Try again in {st.locked_until - now} s.")
            return
        if not arg:
            self.reply(st.chat, "Usage: /auth <password>")
            return
        if hmac.compare_digest(arg.encode(), self.password):
            st.fails = 0
            st.session_until = now + self.session_s
            log(f"chat {st.chat} authenticated")
            self.reply(st.chat,
                       f"Authenticated. Session expires after {self.session_s} s of inactivity.")
            return

        st.fails += 1
        log(f"failed login for chat {st.chat} ({st.fails}/{MAX_FAILS})")
        if st.fails >= MAX_FAILS:
            st.fails = 0
            st.session_until = 0
            st.locked_until = now + LOCKOUT_S
            self.reply(st.chat,
                       f"Wrong password. Too many failed attempts; locked for {LOCKOUT_S} s.")
        else:
            self.reply(st.chat, f"Wrong password ({st.fails}/{MAX_FAILS} failed attempts).")

    def cmd_status(self, chat):
        up = int(time.time() - self.started)
        self.reply(chat,
                   f"Super_RCT {VERSION}\n"
                   f"Host: {socket.gethostname() or 'unknown'}\n"
                   f"PID: {os.getpid()}\n"
                   f"Uptime: {up // 3600}h {(up % 3600) // 60}m {up % 60}s\n"
                   f"IP entries: {len(self.registry.entries)}")

    def cmd_ip(self, chat, args):
        tokens = args.split()
        sub = tokens[0] if tokens else ""
        ip_in = tokens[1] if len(tokens) > 1 else ""
        reg = self.registry

        if sub in ("", "list"):
            if not reg.entries:
                self.reply(chat, "IP registry is empty.")
                return
            lines = "".join(f"{ip} {'enabled' if en else 'disabled'}\n"
                            for ip, en in reg.entries.items())
            self.reply(chat, lines)
            return

        if sub in ("enable", "disable"):
            if not ip_in:
                self.reply(chat, f"Usage: /ip {sub} <ip>")
                return
            ip = reg.normalise(ip_in)
            if ip is None:
                self.reply(chat, f"Invalid IP address: {ip_in}")
                return
            if ip not in reg.entries and len(reg.entries) >= MAX_IPS:
                self.reply(chat, f"IP registry is full ({MAX_IPS} entries).")
                return
            reg.entries[ip] = sub == "enable"
            if not reg.save():
                log(f"failed to write {reg.path}")
                self.reply(chat, "Could not save the IP registry. See the bot log.")
                return
            self.reply(chat, f"{ip} is now {sub}d.")
            return

        self.reply(chat, "Usage: /ip list | /ip enable <ip> | /ip disable <ip>")

    def run_table(self, chat, name, kind, target):
        """Runs a table command: a fixed file or a fixed program, never a shell."""
        note = ""
        if kind == "f":
            try:
                with open(target, "rb") as f:
                    data = f.read(READ_MAX)
            except OSError:
                self.reply(chat, f"/{name}\nNot available on this host: {target}")
                return
        else:
            try:
                proc = subprocess.run(target.split(), stdin=subprocess.DEVNULL,
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                      timeout=CMD_TIMEOUT_S)
                data, status = proc.stdout, proc.returncode
            except FileNotFoundError:
                data, status = b"command not available on this host\n", 127
            except subprocess.TimeoutExpired as err:
                data, status = (err.stdout or b""), 124
            data = data[:READ_MAX]
            if status == 124:
                note = "(timed out)"
            elif status not in (0, 127):
                note = f"(exit {status})"

        body = data.rstrip(b" \t\r\n").decode("utf-8", "replace")
        if note:
            body += "\n" + note
        if not body:
            body = "(no output)"

        raw = body.encode()
        cut = utf8_cut(raw, OUT_MAX)
        if cut < len(raw):
            body = raw[:cut].decode("utf-8", "replace") + "\n... (truncated)"
        self.reply(chat, f"/{name}\n{body}")

    def cmd_commands(self, chat):
        lines = [f"Commands ({len(COMMANDS)}):\n"]
        lines += [f"/{name} - {desc}\n" for name, _kind, _target, desc in COMMANDS]
        for chunk in chunk_lines("".join(lines)):
            self.reply(chat, chunk)

    def handle_message(self, chat, text):
        st = self.chats.get(chat)
        if st is None:
            log(f"ignored message from unauthorised chat {chat}")
            return

        # Same split as the C variant: first word is the command, the rest
        # (internal spaces kept) is the argument, so passwords may contain spaces.
        stripped = text.lstrip(" \t")
        i = 0
        while i < len(stripped) and stripped[i] not in " \t":
            i += 1
        cmd = stripped[:i]
        arg = stripped[i:].strip(" \t\r\n")

        if not cmd.startswith("/"):
            return
        cmd = cmd.split("@", 1)[0]  # groups send /cmd@BotName
        log(f"command {cmd} from chat {chat}")

        now = int(time.time())
        if cmd in ("/start", "/help"):
            self.reply(chat, HELP_TEXT)
        elif cmd == "/auth":
            self.cmd_auth(st, arg, now)
        elif cmd == "/logout":
            st.session_until = 0
            self.reply(chat, "Logged out.")
        elif st.session_until <= now:
            self.reply(chat, "Not authenticated. Send /auth <password> first.")
        else:
            st.session_until = now + self.session_s  # sliding idle timeout
            if cmd == "/status":
                self.cmd_status(chat)
            elif cmd == "/ip":
                self.cmd_ip(chat, arg)
            elif cmd == "/commands":
                self.cmd_commands(chat)
            else:
                entry = next((c for c in COMMANDS if c[1] != "b" and c[0] == cmd[1:]), None)
                if entry is None:
                    self.reply(chat, "Unknown command. Send /help for the list.")
                else:
                    self.run_table(chat, *entry[:3])

    def handle_update(self, update):
        uid = update.get("update_id")
        if isinstance(uid, int) and uid >= self.offset:
            self.offset = uid + 1

        msg = update.get("message")
        if not isinstance(msg, dict):
            return
        chat_obj = msg.get("chat")
        chat = chat_obj.get("id") if isinstance(chat_obj, dict) else None
        text = msg.get("text")
        if isinstance(chat, int) and isinstance(text, str):
            self.handle_message(chat, text)

    def poll_once(self):
        try:
            resp = self.tg.call("getUpdates",
                                {"timeout": POLL_TIMEOUT_S, "offset": self.offset},
                                HTTP_TIMEOUT_S)
        except (OSError, ValueError) as err:
            log(f"getUpdates failed ({err}); retrying in {RETRY_DELAY_S} s")
            time.sleep(RETRY_DELAY_S)
            return

        if not resp.get("ok"):
            log(f"Telegram returned an error: {resp.get('description') or '(no description)'}")
            time.sleep(RETRY_DELAY_S)  # back off on API errors
            return
        for update in resp.get("result") or []:
            if isinstance(update, dict):
                self.handle_update(update)

    def run(self):
        log(f"Super_RCT {VERSION} started ({len(self.chats)} allowed chat(s), "
            f"{len(self.registry.entries)} IP entries)")
        try:
            while True:
                self.poll_once()
        except Stop:
            pass
        log("Super_RCT stopped")


def main(argv):
    if len(argv) > 1:
        if argv[1] in ("--help", "-h"):
            print(usage_text(), end="")
            return 0
        if argv[1] == "--version":
            print(f"super_rct {VERSION}")
            return 0
        print(usage_text(), end="", file=sys.stderr)
        return 2

    try:
        cfg = load_config(os.environ)
    except ConfigError as err:
        print(f"error: {err}", file=sys.stderr)
        print(usage_text(), end="", file=sys.stderr)
        return 2

    def on_signal(signum, frame):
        raise Stop()

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)
    Bot(cfg).run()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
