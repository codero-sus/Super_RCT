# Super_RCT - Android edition (Java)

A Telegram bot that runs as a silent background service on an Android phone and
answers commands about the device. There is no screen: the app has no launcher
icon and no settings page. It starts itself after every reboot and keeps running.

It is the Java edition of Super_RCT, with the same settings and login rules as
the C and Python editions.

## The secrets are inside the APK

The bot token, the allowed chat IDs (your user/chat ID), the login password and
the idle session time are compiled into the APK at build time. You enter them
once, in the terminal, and `builder.py` stores them in
`android/app/secrets.properties`. That file is git-ignored and never committed.

Because the values live in the APK, treat the APK itself as a secret. Anyone
who has the file can read the token and password out of it. Do not upload it or
send it to anyone. If it leaks, revoke the token with @BotFather and rebuild.

The service starts only if the token, at least one chat ID and the password are
present. If any is missing it logs `not started` and stops instead of running
with broken settings.

## Build and install on Windows

1. Double-click `setup.bat` in the repository root (once). It installs Java 17,
   the Android SDK (platform 34, build-tools 34.0.0, platform-tools) and Gradle
   if they are missing, and creates the Gradle wrapper in `android\`.
   Open a new terminal afterwards.
2. `python builder.py` asks for the token, the chat IDs, the password (typed
   twice, hidden) and the session seconds, saves them, runs the unit tests if
   you pass `--test`, and builds the debug APK into `dist\`.
3. Turn on USB debugging on the phone, connect it by USB and accept the prompt,
   then `python builder.py --install`. That installs the APK and starts the
   service right away, so you do not have to reboot to test it.

Options:

| Option | What it does |
| --- | --- |
| `--test` | run the core unit tests before building |
| `--install` | install on the USB-connected phone and start the service |
| `--serial <id>` | pick one phone when several are connected |
| `--no-build` | skip the build and reuse the last APK |
| `--reset-secrets` | enter the token, chat IDs and password again |
| `--check` | report which tools and secrets were found, values hidden |

## Use it

1. Create a bot with @BotFather and copy the token.
2. Find your chat ID: send a message to the bot, then open
   `https://api.telegram.org/bot<token>/getUpdates` and read `chat.id`.
3. Build and install as above. There is nothing to open on the phone.
4. In Telegram, send `/auth <password>`, then any command.

Three wrong passwords lock a chat out for 300 seconds. A login ends after the
idle session time (default 3600 s).

### Seeing what it is doing

The service has no screen, so its output goes to the system log. With the phone
connected by USB:

```
adb -s <phone-id> logcat -s SuperRct
```

The notification is hidden on purpose. Android only shows it if you allow
notifications for the app, which it never asks for.

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

## Get an APK from GitHub Actions

GitHub Actions builds one on every push that touches `android/`: open the
**Android edition (Java)** workflow run and download the `super_rct-android-debug`
artifact. That build runs without a `secrets.properties` file, so the APK it
produces has no token and the service will not start. Use it to check that the
build works, not to run the bot. To get a working APK, build it yourself with
`builder.py`, which enters your secrets.

## Limits compared with the C and Python editions

- Android hides parts of `/proc` from apps. A missing reading shows
  "not available on this device" instead of failing.
- The Android edition has no IP registry, alerts, reports, reminders, watched
  files, or shell commands. Those stay in the C and Python editions.
- The service is a foreground service of type `dataSync`, which is what Android
  requires to keep working in the background.

## Project layout

- `core/` - plain Java library (settings, login, commands, Telegram client,
  `/proc` readers). Unit tests run on the JVM: `gradle :core:test`.
- `app/` - the Android app: `BotService` (silent foreground service),
  `BootReceiver` (starts it at boot) and the manifest with no launcher activity.
- `app/secrets.properties` - the token, chat IDs and password, written by
  `builder.py`. Git-ignored.

Build with Gradle 8.9 and JDK 17 (`cd android && gradle :core:test`). The
Android SDK (platform 34) is needed for `:app:assembleDebug`.