package rct.android;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

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

/**
 * Silent background bot. It starts at boot (see BootReceiver), has no screen, and
 * logs only to logcat (tag SuperRct). The settings were compiled into the APK from
 * android/app/secrets.properties by builder.py.
 */
public final class BotService extends Service {
    private static final String TAG = "SuperRct";
    private static final String CHANNEL = "super_rct_bot";
    private static final int NOTIFICATION_ID = 1;
    private static final String API_BASE = "https://api.telegram.org";
    private static final long RETRY_DELAY_MS = 5000;

    private static volatile boolean running;
    private volatile boolean stopRequested;

    static boolean isRunning() {
        return running;
    }

    private static void log(String line) {
        Log.i(TAG, line);
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        // Android requires startForeground soon after a foreground-service start.
        // The type is passed explicitly: on Android 14 it must match the manifest.
        ensureChannel();
        startForeground(NOTIFICATION_ID, notification(),
                android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        if (running) {
            return START_STICKY;
        }

        Config config = loadConfig();
        if (config == null) {
            stopForeground(STOP_FOREGROUND_REMOVE);
            stopSelf();
            return START_NOT_STICKY;
        }

        running = true;
        stopRequested = false;
        new Thread(() -> poll(config), "super-rct-poll").start();
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        stopRequested = true;
        super.onDestroy();
    }

    /** The settings compiled into this APK, or null (logged) when they are missing or wrong. */
    private static Config loadConfig() {
        Map<String, String> env = new HashMap<>();
        env.put("SUPER_RCT_TOKEN", BuildConfig.BOT_TOKEN);
        env.put("SUPER_RCT_CHAT_IDS", BuildConfig.CHAT_IDS);
        env.put("SUPER_RCT_PASSWORD", BuildConfig.PASSWORD);
        env.put("SUPER_RCT_SESSION_SECONDS", BuildConfig.SESSION_SECONDS);
        try {
            return Config.fromMap(env);
        } catch (ConfigException e) {
            // The message names the missing setting but never prints a value.
            Log.e(TAG, "not started: " + e.getMessage() + " (rebuild with builder.py)");
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
                    config.getChatIds(), commands, BotService::log);
            log("started, allowed chats: " + config.getChatIds().size());

            while (!stopRequested) {
                try {
                    engine.pollOnce(25);
                } catch (IOException | RuntimeException e) {
                    Log.w(TAG, "network problem, retrying in 5 s: " + e.getMessage());
                    sleepQuietly(RETRY_DELAY_MS);
                }
            }
        } finally {
            running = false;
            log("stopped");
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

    private void ensureChannel() {
        if (Build.VERSION.SDK_INT >= 26) {
            // Lowest importance: no sound, no heads-up, no icon in the status bar.
            NotificationChannel channel = new NotificationChannel(CHANNEL, "Background service",
                    NotificationManager.IMPORTANCE_MIN);
            channel.setShowBadge(false);
            getSystemService(NotificationManager.class).createNotificationChannel(channel);
        }
    }

    private Notification notification() {
        Notification.Builder builder = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(this, CHANNEL)
                : new Notification.Builder(this).setPriority(Notification.PRIORITY_MIN);
        return builder.setContentTitle("Background service")
                .setSmallIcon(android.R.drawable.stat_sys_download_done)
                .setOngoing(true)
                .build();
    }
}
