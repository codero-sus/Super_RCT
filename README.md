# Super_RCT

Remote Control Tool via Telegram and Discord, with an IP address registry
(enable/disable) and a `/run` command that runs shell commands on the host.

Super_RCT is a small Telegram bot with two interchangeable implementations that
share the same commands, replies and configuration:

- `src/super_rct.c`: C, for Linux, macOS, BSD and Windows. The Linux release binary is about 28 KB.
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

### Read-only system commands (751)

After `/auth`, these commands return live information from the host. Each one
is either a fixed file read or a fixed program with fixed arguments. None
accepts user input, and none runs through a shell. The one exception is `/run`,
described below.

| Group | Examples |
| --- | --- |
| Memory, CPU, kernel | `/meminfo`, `/cpuinfo`, `/loadavg`, `/kversion`, `/pressure_mem`, `/vmstat` |
| Hardware and power | `/lsblk`, `/df`, `/free`, `/lsusb`, `/lspci`, `/sensors`, `/thermal`, `/battery` |
| Kernel tunables | `/sys_swappiness`, `/sys_ip_forward`, `/sys_randomize`, `/sys_file_max` |
| Network | `/ip_addr`, `/ip_route`, `/ss_listen`, `/ss_established`, `/net_tcp`, `/net_dev` |
| Processes and services | `/ps`, `/ps_mem`, `/top`, `/systemd_running`, `/systemd_failed`, `/journal_errors` |
| Users, time, identity | `/date`, `/uptime`, `/who`, `/whoami`, `/os_release`, `/machine_id`, `/timezone` |

Most of the commands in the `sys_*` group read kernel tunables under `/proc/sys`
(386 of them). Commands for files or programs that a host does not have answer
"Not available on this host" instead of failing.

Every command is defined in `src/commands.def`. The C and Python tables are
generated from that file (`make gen`). The test suite fails if they are stale.

Commands run with a 5-second time limit. Output is capped at about 3.5 KB and
sent in several messages if a listing is long.

Adding a command means adding one line to `src/commands.def` and running
`make gen`. Table commands never run through a shell. `/run` is the separate, explicit
shell command described in the next section.

The IP registry is stored in a plain text file (`super_rct_ips.txt` by default).
It is informational: Super_RCT does not change firewall rules.

## Running shell commands: /run

`/run <command>` runs the text through `/bin/sh` on the host, from any chat that
has logged in with `/auth`, on Telegram and Discord alike. It is **on by default**.
Anyone with the password can run any command as the user the bot runs as, so run
the bot as a normal user unless it truly needs root.

- Command text: up to 1000 bytes.
- Time limit: 60 seconds. A command that runs longer is killed together with the
  background processes it started.
- Output: the first 3500 characters are sent, with the exit code.
- Audit log: each run is written to stderr with the chat ID, exit status and
  command text. Output is not logged. Commands can contain secrets, so the log
  should be protected.
- Turn it off with `SUPER_RCT_SHELL=0`. Any value other than `0` or `1` stops
  the bot at startup.

Passwords and commands typed in chat stay in the chat history of Telegram or
Discord.

## Monitoring and utilities (29)

All of these need `/auth` first. Both variants give the same replies, except that `/about` names the variant.

- **Health**: `/health` (summary), `/diagnose` (each check marked OK, WARN or
  FAIL, with a result line), `/mem`, `/swap`, `/disk`, `/load`, `/temp`,
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

Alert limits and report schedules are saved to `SUPER_RCT_STATE_FILE` (default
`super_rct_state.txt`) after each change, written with mode 0600, and restored
on restart. Reminders and counters are kept in memory and reset when the bot
restarts. Nothing here captures the screen, camera, microphone,
keyboard or clipboard. The only shell access is `/run` (see below).

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

On Windows, build with `tools/builder.py --target c` (see below). It needs MinGW
gcc on PATH, or `pip install ziglang`, which bundles a C compiler. Windows 10 and
later include `curl.exe`.

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
make test       # bot tests against the C binary and the Python script, then installer tests
```

## Discord

Discord is optional and runs next to Telegram. It uses the same `/auth`
password and the same commands.

- Create an application in the Discord Developer Portal, add a bot, and copy
  its token into `SUPER_RCT_DISCORD_TOKEN`.
- Under Bot, enable the **Message Content** privileged intent. Without it the
  bot cannot read command text.
- Invite the bot with *Read Message History* and *Send Messages* on the target
  channel. Put that channel ID in `SUPER_RCT_DISCORD_CHANNEL_IDS`.
- Use a private channel. Anyone who can see the channel can see the password
  if it is typed there, and the bot's replies.
- Channel IDs must not overlap with `SUPER_RCT_CHAT_IDS`.

## Build an executable

Run the builder. With no arguments on a terminal it asks each question in turn:

```sh
python3 tools/builder.py
```

Or pass arguments to build without prompts:

```sh
python3 tools/builder.py --target c            # C binary, build/dist/super_rct
python3 tools/builder.py --target py-nuitka    # Python onefile executable, build/dist/
python3 tools/builder.py --target installers   # Linux installers, build/dist/
python3 tools/builder.py --list                # list the targets
python3 tools/builder.py --help                # every option (--out, --name, --dry-run)
```

For a window instead of a terminal, run `python3 tools/gui_builder.py`. It uses
tkinter, which comes with Python on Windows and macOS. On Linux install it first,
for example with `sudo apt install python3-tk`.

`--dry-run` shows the steps without building. The Nuitka target (`py-nuitka`)
needs Nuitka and Python development headers. Run the builder from an environment
that has Nuitka installed. Each target builds for the operating system it runs on.

## Install on your own Linux machine

`build/installers/super_rct-linux` (C) and `build/installers/super_rct-linux.py`
(Python) are built by `python3 tools/build_installers.py`. Each one bundles the
bot. Run it on the machine you own:

```sh
./super_rct-linux install      # asks for the tokens, chat IDs and password
./super_rct-linux uninstall    # stops the service and removes what install added
```

- Install writes the bot files, `super_rct.env` (mode 0600, your secrets) and a
  manifest `super_rct_install.txt` into the current directory. It refuses to
  overwrite existing files.
- It registers a visible systemd user service, `super-rct.service`, under
  `~/.config/systemd/user/`. Where systemd user services are not available, it
  writes the files and tells you to start the bot yourself.
- Uninstall removes only the files listed in the manifest, then the manifest.
  The installer file stays where it is, so you can see it and delete it.
- The installer does not hide itself and does not copy itself anywhere else.
- Windows and macOS installers are not in this repository yet.

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
| `SUPER_RCT_STATE_FILE` | no | `super_rct_state.txt` |
| `SUPER_RCT_API` | no | `https://api.telegram.org` |
| `SUPER_RCT_WATCH_PATHS` | no (comma-separated absolute paths, max 16) | - |
| `SUPER_RCT_WATCH_MAX_AGE_HOURS` | no (1-8760) | `24` |
| `SUPER_RCT_SHELL` | no (0 or 1) | `1` (`/run` enabled) |

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

## Author and credits

Super_RCT is made by **codero-sus**.

- GitHub: [github.com/codero-sus](https://github.com/codero-sus)
- Website: [the-codero.web.app](https://the-codero.web.app/)

Made with ❤️ by codero-sus
