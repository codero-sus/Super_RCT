"""Discord transport for Super_RCT (Python variant).

Uses the Discord REST API with polling, so no gateway or third-party package
is needed. The bot must have the "Message Content" privileged intent enabled
in the developer portal, or message text arrives empty.
"""

import json
import time
import urllib.error
import urllib.request

DEFAULT_DISCORD_API = "https://discord.com/api/v10"
MAX_MESSAGE = 1900  # Discord allows 2000 characters; leave room
USER_AGENT = "DiscordBot (https://github.com/codero-sus/Super_RCT, 0.5.0)"


class DiscordError(Exception):
    pass


class Discord:
    def __init__(self, token, api=DEFAULT_DISCORD_API):
        self.token = token
        self.api = api.rstrip("/")
        self.cursor = {}  # channel id -> newest message id seen

    def _call(self, method, path, body=None, timeout=15):
        data = None if body is None else json.dumps(body).encode()
        headers = {"Authorization": f"Bot {self.token}", "User-Agent": USER_AGENT,
                   "Content-Type": "application/json"}
        for attempt in range(2):
            req = urllib.request.Request(self.api + path, data=data, method=method,
                                         headers=headers)
            try:
                with urllib.request.urlopen(req, timeout=timeout) as resp:
                    return json.loads(resp.read().decode("utf-8", "replace") or "null")
            except urllib.error.HTTPError as err:
                if err.code == 429 and attempt == 0:
                    # Honour retry_after once, capped, then give up.
                    try:
                        wait = float(json.loads(err.read()).get("retry_after", 1))
                    except (ValueError, TypeError, AttributeError):
                        wait = 1.0
                    time.sleep(min(max(wait, 0.0), 30.0))
                    continue
                raise DiscordError(f"HTTP {err.code}") from None
        raise DiscordError("rate limited")

    def send(self, channel, text):
        for start in range(0, max(len(text), 1), MAX_MESSAGE):
            self._call("POST", f"/channels/{channel}/messages",
                       {"content": text[start:start + MAX_MESSAGE] or "(no output)"})

    def poll(self, channel):
        """Returns [text] for new human messages in `channel`, oldest first.

        The first call only records the newest message, so old history is
        never replayed as commands.
        """
        if channel not in self.cursor:
            newest = self._call("GET", f"/channels/{channel}/messages?limit=1") or []
            self.cursor[channel] = int(newest[0]["id"]) if newest else 0
            return []
        msgs = self._call("GET", f"/channels/{channel}/messages"
                                 f"?after={self.cursor[channel]}&limit=50") or []
        texts = []
        for msg in sorted(msgs, key=lambda m: int(m["id"])):
            self.cursor[channel] = max(self.cursor[channel], int(msg["id"]))
            author = msg.get("author") or {}
            if not author.get("bot"):
                texts.append(msg.get("content") or "")
        return texts
