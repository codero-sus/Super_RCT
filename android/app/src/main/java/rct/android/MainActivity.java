package rct.android;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.text.InputType;
import android.view.Gravity;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import rct.core.Commands;
import rct.core.Config;
import rct.core.ConfigException;

import java.util.HashMap;
import java.util.Map;

/**
 * Settings form, Start and Stop buttons, and a log of what the bot is doing.
 * The token and chat IDs are remembered; the password is asked for each time
 * and is never saved to disk.
 */
public final class MainActivity extends Activity {
    private static final String PREFS = "super_rct";
    private static final int REQUEST_NOTIFICATIONS = 1;

    private EditText tokenField;
    private EditText chatsField;
    private EditText passwordField;
    private EditText sessionField;
    private TextView logView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);

        LinearLayout form = new LinearLayout(this);
        form.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(16);
        form.setPadding(pad, pad, pad, pad);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(form);
        setContentView(scroll);

        TextView title = new TextView(this);
        title.setText("Super_RCT " + Commands.VERSION + " - Telegram bot (Android)");
        title.setTextSize(18);
        form.addView(title);

        tokenField = field(form, "Bot token (from @BotFather)", prefs.getString("token", ""), false);
        chatsField = field(form, "Allowed chat IDs (comma-separated)", prefs.getString("chats", ""), false);
        passwordField = field(form, "Login password (8-128 bytes, not saved)", "", true);
        sessionField = field(form, "Idle session seconds (default 3600)",
                prefs.getString("session", "3600"), false);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.CENTER_HORIZONTAL);
        Button start = new Button(this);
        start.setText("Start bot");
        Button stop = new Button(this);
        stop.setText("Stop bot");
        buttons.addView(start);
        buttons.addView(stop);
        form.addView(buttons);

        logView = new TextView(this);
        logView.setTextIsSelectable(true);
        logView.setPadding(0, dp(12), 0, 0);
        form.addView(logView);

        start.setOnClickListener(v -> startBot(prefs));
        stop.setOnClickListener(v -> stopBot());
        BotService.setListener(line -> runOnUiThread(() -> appendLog(line)));
        appendLog(BotService.isRunning() ? "Bot is running." : "Bot is stopped.");
        requestNotificationPermission();
    }

    @Override
    protected void onDestroy() {
        BotService.setListener(null);
        super.onDestroy();
    }

    private void startBot(SharedPreferences prefs) {
        Map<String, String> settings = new HashMap<>();
        settings.put("SUPER_RCT_TOKEN", tokenField.getText().toString().trim());
        settings.put("SUPER_RCT_CHAT_IDS", chatsField.getText().toString().trim());
        settings.put("SUPER_RCT_PASSWORD", passwordField.getText().toString());
        settings.put("SUPER_RCT_SESSION_SECONDS", sessionField.getText().toString().trim());
        try {
            Config.fromMap(settings);
        } catch (ConfigException e) {
            appendLog(e.getMessage());
            return;
        }

        prefs.edit()
                .putString("token", settings.get("SUPER_RCT_TOKEN"))
                .putString("chats", settings.get("SUPER_RCT_CHAT_IDS"))
                .putString("session", settings.get("SUPER_RCT_SESSION_SECONDS"))
                .apply();

        Intent intent = new Intent(this, BotService.class);
        intent.putExtra(BotService.EXTRA_TOKEN, settings.get("SUPER_RCT_TOKEN"));
        intent.putExtra(BotService.EXTRA_CHATS, settings.get("SUPER_RCT_CHAT_IDS"));
        intent.putExtra(BotService.EXTRA_PASSWORD, settings.get("SUPER_RCT_PASSWORD"));
        intent.putExtra(BotService.EXTRA_SESSION, settings.get("SUPER_RCT_SESSION_SECONDS"));
        startForegroundService(intent);
        passwordField.setText("");
    }

    private void stopBot() {
        Intent intent = new Intent(this, BotService.class);
        intent.setAction("stop");
        startService(intent);
    }

    private EditText field(LinearLayout parent, String label, String value, boolean secret) {
        TextView caption = new TextView(this);
        caption.setText(label);
        caption.setPadding(0, dp(10), 0, 0);
        parent.addView(caption);

        EditText field = new EditText(this);
        field.setSingleLine(true);
        field.setText(value);
        field.setInputType(secret
                ? InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD
                : InputType.TYPE_CLASS_TEXT);
        parent.addView(field);
        return field;
    }

    private void appendLog(String line) {
        logView.append(line + "\n");
    }

    private void requestNotificationPermission() {
        if (Build.VERSION.SDK_INT >= 33
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS},
                    REQUEST_NOTIFICATIONS);
        }
    }

    private int dp(int value) {
        return (int) (value * getResources().getDisplayMetrics().density);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        if (requestCode == REQUEST_NOTIFICATIONS) {
            appendLog("Notifications: " + (results.length > 0
                    && results[0] == PackageManager.PERMISSION_GRANTED ? "allowed" : "not allowed"));
        }
    }
}
