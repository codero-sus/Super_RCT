# Super_RCT

Remote Control Tool via Telegram, with an IP address registry (enable/disable).

Super_RCT is a small Telegram bot with two interchangeable implementations that
share the same commands, replies and configuration:

- `src/super_rct.c`: C, POSIX. The release binary is about 28 KB.
- `src/super_rct.py`: Python 3.8+, standard library only.

Both long-poll the Telegram Bot API, only talk to chats you list, and require a
password login before running any command.

## Features

| Command | What it does |
| --- | --- |
| `/auth <password>` | Log in for your chat (required first) |
| `/logout` | End your login |
| `/help` | List commands (works before login) |
| `/status` | Host name, PID, uptime, registry size |
| `/ip list` | Show the IP registry |
| `/ip enable <ip>` | Mark an IPv4/IPv6 address as enabled |
| `/ip disable <ip>` | Mark an IPv4/IPv6 address as disabled |

The IP registry is stored in a plain text file (`super_rct_ips.txt` by default).
It is informational: Super_RCT does not change firewall rules. Arbitrary shell
execution is deliberately not offered.

## Authentication

Every chat must first send `/auth <password>`. Then:

- A login stays valid while you keep using the bot. It expires after
  `SUPER_RCT_SESSION_SECONDS` (default 3600) of inactivity.
- Three wrong passwords lock that chat out of `/auth` for 300 seconds.
- Passwords are compared in constant time and are never written to the log.
- Telegram keeps the message you typed. Delete your `/auth` message in the
  chat after logging in.

## Build

### C variant

Requirements: a C11 compiler, `make`, a POSIX system (Linux, macOS, BSD), and
the `curl` command-line tool (used for HTTPS).

```sh
make            # debug-friendly build -> build/super_rct
make release    # stripped, size-optimised build -> build/super_rct
make size       # print the release binary size
```

### Python variant

No build step. Requires Python 3.8+ and the `curl` tool is not needed.

```sh
python3 src/super_rct.py
```

### Tests

```sh
make test       # runs the same 12 tests against the C binary and the Python script
```

## Setup

1. Create a bot with [@BotFather](https://t.me/BotFather) and copy its token.
2. Find your chat ID (for example by messaging your bot and reading the
   `chat.id` in `getUpdates`).
3. Run the bot:

```sh
export SUPER_RCT_TOKEN='123456789:your-token-here'
export SUPER_RCT_CHAT_IDS='123456789'        # comma-separated for several chats
export SUPER_RCT_PASSWORD='a long passphrase'
./build/super_rct            # or: python3 src/super_rct.py
```

Environment variables:

| Variable | Required | Default |
| --- | --- | --- |
| `SUPER_RCT_TOKEN` | yes | - |
| `SUPER_RCT_CHAT_IDS` | yes | - |
| `SUPER_RCT_PASSWORD` | yes (8-128 bytes) | - |
| `SUPER_RCT_SESSION_SECONDS` | no (5-86400) | `3600` |
| `SUPER_RCT_IP_FILE` | no | `super_rct_ips.txt` |
| `SUPER_RCT_API` | no | `https://api.telegram.org` |

Keep the token out of version control. Put it in your shell environment or a
secrets manager, not in a committed file.

## Design

- **Small**: the C build has no runtime libraries beyond libc.
- **Private**: the bot token is passed to curl through stdin (`curl -K -`), so
  it does not appear in `ps` output.
- **Safe parsing**: a depth-aware JSON reader reads only top-level fields, so a
  quoted `text` inside a reply cannot be executed as a command.
- **Resilient**: API errors are logged and retried every 5 seconds. Ctrl-C or
  SIGTERM stops the bot cleanly.
- **Non-blocking mistakes**: a failed login is counted per chat, so one chat's
  typos cannot lock out another.

## Layout

```
src/super_rct.c         the bot, C variant
src/super_rct.py       the bot, Python variant (same behaviour)
Makefile               build, release, test, size
tests/test_super_rct.py integration tests (both variants) against a mock Telegram API
```
