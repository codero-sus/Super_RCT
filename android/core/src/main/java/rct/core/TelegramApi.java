package rct.core;

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

/** Telegram Bot API over HTTPS (HttpURLConnection, which Android provides). */
public final class TelegramApi implements BotEngine.Api {
    private static final int CONNECT_TIMEOUT_MS = 15_000;
    private static final int SEND_TIMEOUT_MS = 20_000;

    private final String baseUrl; // e.g. https://api.telegram.org/bot<token>/

    /** baseUrl is normally https://api.telegram.org; the token is never logged. */
    public TelegramApi(String baseUrl, String token) {
        this.baseUrl = baseUrl + "/bot" + token + "/";
    }

    @Override
    public String getUpdates(long offset, int timeoutSeconds) throws IOException {
        JsonObject body = new JsonObject();
        body.addProperty("offset", offset);
        body.addProperty("timeout", timeoutSeconds);
        JsonArray allowed = new JsonArray();
        allowed.add("message");
        body.add("allowed_updates", allowed);
        // Read timeout must exceed the server-side long-poll wait.
        return post("getUpdates", body.toString(), (timeoutSeconds + 15) * 1000);
    }

    @Override
    public void sendMessage(long chat, String text) throws IOException {
        JsonObject body = new JsonObject();
        body.addProperty("chat_id", chat);
        body.addProperty("text", text);
        String response = post("sendMessage", body.toString(), SEND_TIMEOUT_MS);
        JsonObject result = JsonParser.parseString(response).getAsJsonObject();
        if (!result.has("ok") || !result.get("ok").getAsBoolean()) {
            String reason = result.has("description") ? result.get("description").getAsString() : "";
            throw new IOException("sendMessage failed: " + reason);
        }
    }

    private String post(String method, String json, int readTimeoutMs) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(baseUrl + method).openConnection();
        try {
            connection.setRequestMethod("POST");
            connection.setConnectTimeout(CONNECT_TIMEOUT_MS);
            connection.setReadTimeout(readTimeoutMs);
            connection.setDoOutput(true);
            connection.setRequestProperty("Content-Type", "application/json; charset=utf-8");
            try (OutputStream out = connection.getOutputStream()) {
                out.write(json.getBytes(StandardCharsets.UTF_8));
            }
            int code = connection.getResponseCode();
            InputStream in = code >= 400 ? connection.getErrorStream() : connection.getInputStream();
            return in == null ? "" : readAll(in);
        } finally {
            connection.disconnect();
        }
    }

    private static String readAll(InputStream in) throws IOException {
        try (InputStream input = in) {
            ByteArrayOutputStream buffer = new ByteArrayOutputStream();
            byte[] chunk = new byte[4096];
            int n;
            while ((n = input.read(chunk)) != -1) {
                buffer.write(chunk, 0, n);
            }
            return new String(buffer.toByteArray(), StandardCharsets.UTF_8);
        }
    }
}
