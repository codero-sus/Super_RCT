package rct.android;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;

import rct.core.Auth;
import rct.core.BotEngine;
import rct.core.Commands;
import rct.core.Config;
import rct.core.ConfigException;
import rct.core.ProcHost;
import rct.core.TelegramApi;

import java.io.IOException;
import java.util.HashMap;
import java.util.Map;
import java.util.function.Consumer;

/**
 * Runs the Telegram long-poll loop in the background as a foreground service, so
 * the bot keeps answering while the app is not on screen.
 */
public final class BotService extends Service {
    static final String EXTRA_TOKEN = "token";
    static final String EXTRA_CHATS = "chats";
    static final String EXTRA_PASSWORD = "password";
    static final String EXTRA_SESSION = "session";

    private static final String CHANNEL = "super_rct_bot";
    private static final int NOTIFICATION_ID = 1;
    private static final String API_BASE = "https://api.telegram.org";
    private static final long RETRY_DELAY_MS = 5000;

    private static volatile Consumer<String> listener;
    private static volatile boolean running;

    private volatile boolean stopRequested;

    /** The activity registers here to show log lines. Null means nobody is listening. */
    static void setListener(Consumer<String> newListener) {
        listener = newListener;
    }

    static boolean isRunning() {
        return running;
    }

    static void emit(String line) {
        Consumer<String> target = listener;
        if (target != null) {
            target.accept(line);
        }
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && "stop".equals(intent.getAction())) {
            stopRequested = true;
            stopSelf();
            return START_NOT_STICKY;
        }
        // Must call startForeground promptly after startForegroundService, even if we stop soon.
        ensureChannel();
        startForeground(NOTIFICATION_ID, notification("Running"));
        if (running) {
            emit("Already running.");
            return START_NOT_STICKY;
        }

        final Config config = loadConfig(intent);
        if (config == null) {
            stopForeground(STOP_FOREGROUND_REMOVE);
            stopSelf();
            return START_NOT_STICKY;
        }

        running = true;
        stopRequested = false;
        Thread worker = new Thread(() -> poll(config), "super-rct-poll");
        worker.start();
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        stopRequested = true;
        super.onDestroy();
    }

    /** Returns the settings, or null after reporting why they are wrong. */
    private Config loadConfig(Intent intent) {
        try {
            return Config.fromMap(settingsFrom(intent));
        } catch (ConfigException e) {
            emit(e.getMessage());
            return null;
        }
    }

    private void poll(Config config) {
        try {
            Auth auth = new Auth(config.getPassword(), config.getSessionSeconds(),
                    () -> System.currentTimeMillis() / 1000L);
            Commands commands = new Commands(auth, ProcHost.device(),
                    () -> System.currentTimeMillis() / 1000L);
            BotEngine engine = new BotEngine(new TelegramApi(API_BASE, config.getToken()),
                    config.getChatIds(), commands, BotService::emit);
            emit("Bot started. Allowed chats: " + config.getChatIds().size());

            while (!stopRequested) {
                try {
                    engine.pollOnce(25);
                } catch (IOException | RuntimeException e) {
                    emit("Network problem, retrying in 5 s: " + e.getMessage());
                    sleepQuietly(RETRY_DELAY_MS);
                }
            }
        } finally {
            running = false;
            emit("Bot stopped.");
            stopForeground(STOP_FOREGROUND_REMOVE);
            stopSelf();
        }
    }

    private static void sleepQuietly(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private static Map<String, String> settingsFrom(Intent intent) {
        Map<String, String> env = new HashMap<>();
        if (intent == null) {
            return env;
        }
        env.put("SUPER_RCT_TOKEN", intent.getStringExtra(EXTRA_TOKEN));
        env.put("SUPER_RCT_CHAT_IDS", intent.getStringExtra(EXTRA_CHATS));
        env.put("SUPER_RCT_PASSWORD", intent.getStringExtra(EXTRA_PASSWORD));
        env.put("SUPER_RCT_SESSION_SECONDS", intent.getStringExtra(EXTRA_SESSION));
        return env;
    }

    private void ensureChannel() {
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationChannel channel = new NotificationChannel(CHANNEL, "Super_RCT",
                    NotificationManager.IMPORTANCE_LOW);
            getSystemService(NotificationManager.class).createNotificationChannel(channel);
        }
    }

    private Notification notification(String text) {
        Notification.Builder builder = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(this, CHANNEL)
                : new Notification.Builder(this);
        return builder.setContentTitle("Super_RCT")
                .setContentText(text)
                .setSmallIcon(android.R.drawable.stat_notify_sync)
                .setOngoing(true)
                .build();
    }
}
