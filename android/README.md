# Super_RCT - Android edition (Java)

A Telegram bot that runs on an Android phone and answers commands about the
device. It is the Java edition of Super_RCT; the same settings and login rules
as the C and Python editions.

## Get the APK

GitHub Actions builds it: open the **Android edition (Java)** workflow run for
your branch and download the `super_rct-android-debug` artifact. Install the
`.apk` on the phone and allow installs from your file manager when Android asks.
The APK is a debug build, so it is signed with the standard debug key.

## Use it

1. Create a bot with @BotFather and copy the token.
2. Find your chat ID (send a message to the bot and read `chat.id` from
   `https://api.telegram.org/bot<token>/getUpdates`).
3. Open Super_RCT, fill in the token, the allowed chat IDs, and a login password
   (8-128 bytes). Tap **Start bot**. The password is not saved; re-enter it after
   restarting the app.
4. In Telegram, send `/auth <password>`, then any command.

Three wrong passwords lock a chat out for 300 seconds. A login ends after the
idle session time (default 3600 s).

## Commands

| Command | What it does |
| --- | --- |
| `/auth <password>` | log in |
| `/logout` | end your session |
| `/status` | app and device status |
| `/mem` | memory use |
| `/uptime` | device uptime |
| `/load` | load average |
| `/echo <text>` | repeat the text |
| `/commands`, `/help` | list commands |

## Limits compared with the C and Python editions

- Android hides parts of `/proc` from apps. A missing reading shows
  "not available on this device" instead of failing.
- The Android edition has no IP registry, alerts, reports, reminders, watched
  files, or shell commands. Those stay in the C and Python editions.
- The bot runs as a foreground service with a notification while it is on.

## Project layout

- `core/` - plain Java library (settings, login, commands, Telegram client,
  `/proc` readers). Unit tests run on the JVM: `gradle :core:test`.
- `app/` - the Android app (settings screen, foreground service).

Build locally with Gradle 8.9 and JDK 17 (`cd android && gradle :core:test`).
The Android SDK (platform 34) is needed for `:app:assembleDebug`.
