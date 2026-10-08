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
| `/commands` | List every command with a one-line description |

### Read-only system commands (235)

After `/auth`, these commands return live information from the host. Each one
is either a fixed file read or a fixed program with fixed arguments. None
accepts user input, and none runs through a shell.

| Group | Examples |
| --- | --- |
| Memory, CPU, kernel | `/meminfo`, `/cpuinfo`, `/loadavg`, `/kversion`, `/pressure_mem`, `/vmstat` |
| Hardware and power | `/lsblk`, `/df`, `/free`, `/lsusb`, `/lspci`, `/sensors`, `/thermal`, `/battery` |
| Kernel tunables | `/sys_swappiness`, `/sys_ip_forward`, `/sys_randomize`, `/sys_file_max` |
| Network | `/ip_addr`, `/ip_route`, `/ss_listen`, `/ss_established`, `/net_tcp`, `/net_dev` |
| Processes and services | `/ps`, `/ps_mem`, `/top`, `/systemd_running`, `/systemd_failed`, `/journal_errors` |
| Users, time, identity | `/date`, `/uptime`, `/who`, `/whoami`, `/os_release`, `/machine_id`, `/timezone` |

Every command is defined in `src/commands.def`. The C and Python tables are
generated from that file (`make gen`). The test suite fails if they are stale.

Commands run with a 5-second time limit. Output is capped at about 3.5 KB and
sent in several messages if a listing is long.

Adding a command means adding one line to `src/commands.def` and running
`make gen`. Adding a shell command is deliberately not supported. A shell
command would let anyone who gets past `/auth` run anything on the host.

The IP registry is stored in a plain text file (`super_rct_ips.txt` by default).
It is informational: Super_RCT does not change firewall rules. Arbitrary shell
execution is deliberately not offered.

## Monitoring and utilities (28)

All of these need `/auth` first. Both variants give the same replies, except that `/about` names the variant.

- **Health**: `/health` (summary), `/mem`, `/swap`, `/disk`, `/load`, `/temp`,
  `/cpu` (1 s sample), `/net_rate` (1 s sample), `/procs`, `/uptime_h`,
  `/failed_units`, `/updates_count` (apt), `/reboot_required`.
- **Alerts**: `/alert list`, `/alert set mem|disk|temp|load <value>`,
  `/alert off <metric>`, `/alert check`. The bot checks every 60 seconds and
  sends `ALERT` once when a limit is reached and `RECOVERED` once it drops back
  below the limit minus a margin (5 points, or 0.2 for load). Limits: mem and
  disk 1-100 %, temp 20-120 C, load 0.1-100 per core.
- **Reports**: `/report status`, `/report on <5-1440 minutes>`, `/report off`,
  `/report now`.
- **Reminders**: `/remind <1-10080 minutes> <text>`, `/timers`,
  `/timer_cancel <id>`. Up to 10 per chat and 32 in total.
- **Watched files**: `/watch` and `/watch_check` report the age of the paths in
  `SUPER_RCT_WATCH_PATHS` and flag any older than
  `SUPER_RCT_WATCH_MAX_AGE_HOURS`.
- **Utilities**: `/notify <text>` (desktop notification through `notify-send`),
  `/echo`, `/botstats`, `/sessions`, `/logout_all`, `/about`, `/selftest`,
  `/config` (never shows the token or password).

Alert limits, reports, reminders and counters are kept in memory. They reset
when the bot restarts. Nothing here captures the screen, camera, microphone,
keyboard or clipboard, and nothing runs an arbitrary shell command.

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
| `SUPER_RCT_WATCH_PATHS` | no (comma-separated absolute paths, max 16) | - |
| `SUPER_RCT_WATCH_MAX_AGE_HOURS` | no (1-8760) | `24` |

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
- **Per-chat limits**: failed logins are counted per chat, so one chat's
  typos cannot lock out another.

## Layout

```
src/super_rct.c         the bot, C variant
src/super_rct.py       the bot, Python variant (same behaviour)
src/monitor.h          monitoring and utility commands, C variant
src/monitor.py         monitoring and utility commands, Python variant
src/commands.def       command table (source); generates commands_table.h/.py
Makefile               build, release, test, size, gen, check-gen
tests/test_super_rct.py integration tests (both variants) against a mock Telegram API
```
