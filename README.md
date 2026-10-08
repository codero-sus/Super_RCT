# Super_RCT

Remote Control Tool via Telegram, with an IP address registry (enable/disable).

Super_RCT is a small, dependency-free bot written in C. The release binary is
about 24 KB. It long-polls the Telegram Bot API and only obeys chats you list.

## Features

| Command | What it does |
| --- | --- |
| `/help` | List commands |
| `/status` | Host name, PID, uptime, registry size |
| `/ip list` | Show the IP registry |
| `/ip enable <ip>` | Mark an IPv4/IPv6 address as enabled |
| `/ip disable <ip>` | Mark an IPv4/IPv6 address as disabled |

The IP registry is stored in a plain text file (`super_rct_ips.txt` by default).
It is informational: Super_RCT does not change firewall rules. Arbitrary shell
execution is deliberately not offered.

## Build

Requirements: a C11 compiler, `make`, POSIX system (Linux, macOS, BSD), and the
`curl` command-line tool (used for HTTPS).

```sh
make            # debug-friendly build -> build/super_rct
make release    # stripped, size-optimised build -> build/super_rct
make size       # print the release binary size
make test       # integration tests (mock Telegram server, needs python3)
```

## Setup

1. Create a bot with [@BotFather](https://t.me/BotFather) and copy its token.
2. Find your chat ID (for example by messaging your bot and reading the
   `chat.id` in `getUpdates`).
3. Run the bot:

```sh
export SUPER_RCT_TOKEN='123456789:your-token-here'
export SUPER_RCT_CHAT_IDS='123456789'        # comma-separated for several chats
./build/super_rct
```

Environment variables:

| Variable | Required | Default |
| --- | --- | --- |
| `SUPER_RCT_TOKEN` | yes | - |
| `SUPER_RCT_CHAT_IDS` | yes | - |
| `SUPER_RCT_IP_FILE` | no | `super_rct_ips.txt` |
| `SUPER_RCT_API` | no | `https://api.telegram.org` |

Keep the token out of version control. Put it in your shell environment or a
secrets manager, not in a committed file.

## Design

- **Small**: one C file, `src/super_rct.c`, no runtime libraries beyond libc.
- **Private**: the bot token is passed to curl through stdin (`curl -K -`), so
  it does not appear in `ps` output.
- **Safe parsing**: a depth-aware JSON reader reads only top-level fields, so a
  quoted `text` inside a reply cannot be executed as a command.
- **Resilient**: API errors are logged and retried every 5 seconds. Ctrl-C or
  SIGTERM stops the bot cleanly.

## Layout

```
src/super_rct.c        the bot
Makefile               build, release, test, size
tests/test_super_rct.py integration tests against a mock Telegram API
```
