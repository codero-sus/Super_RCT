package rct.core;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParseException;
import com.google.gson.JsonParser;

import java.io.IOException;
import java.util.Set;
import java.util.function.Consumer;

/**
 * Long-poll loop logic: reads Telegram updates, drops chats that are not on the
 * allow-list, runs commands and sends the replies back.
 */
public final class BotEngine {
    /** Body cap per message, under Telegram's 4096-character limit. */
    public static final int OUT_MAX = 3500;

    /** The network side. TelegramApi implements it; tests use a fake. */
    public interface Api {
        /** Returns the raw JSON body of a getUpdates call. */
        String getUpdates(long offset, int timeoutSeconds) throws IOException;

        void sendMessage(long chat, String text) throws IOException;
    }

    private final Api api;
    private final Set<Long> allowed;
    private final Commands commands;
    private final Consumer<String> log;
    private long offset;

    public BotEngine(Api api, Set<Long> allowed, Commands commands, Consumer<String> log) {
        this.api = api;
        this.allowed = allowed;
        this.commands = commands;
        this.log = log;
    }

    /** The next update id to ask for. */
    public long offset() {
        return offset;
    }

    /** One long-poll cycle. Throws IOException when the network or the response fails. */
    public int pollOnce(int timeoutSeconds) throws IOException {
        JsonObject root = parseObject(api.getUpdates(offset, timeoutSeconds));
        if (!root.has("ok") || !root.get("ok").getAsBoolean()) {
            throw new IOException("getUpdates failed");
        }
        JsonArray result = root.has("result") && root.get("result").isJsonArray()
                ? root.getAsJsonArray("result")
                : new JsonArray();
        int handled = 0;
        for (JsonElement element : result) {
            if (element.isJsonObject()) {
                handleUpdate(element.getAsJsonObject());
                handled++;
            }
        }
        return handled;
    }

    private void handleUpdate(JsonObject update) {
        long id;
        try {
            id = update.get("update_id").getAsLong();
        } catch (RuntimeException e) {
            log.accept("skipped an update without a valid update_id");
            return;
        }
        if (id >= offset) {
            offset = id + 1;
        }
        try {
            handleMessage(update);
        } catch (RuntimeException e) {
            log.accept("skipped a bad update: " + e.getMessage());
        }
    }

    private void handleMessage(JsonObject update) {
        JsonElement messageElement = update.get("message");
        if (messageElement == null || !messageElement.isJsonObject()) {
            return;
        }
        JsonObject message = messageElement.getAsJsonObject();
        JsonElement chatElement = message.get("chat");
        JsonElement textElement = message.get("text");
        if (chatElement == null || !chatElement.isJsonObject()
                || textElement == null || !textElement.isJsonPrimitive()
                || !textElement.getAsJsonPrimitive().isString()) {
            return; // stickers, photos and other non-text messages
        }
        long chat = chatElement.getAsJsonObject().get("id").getAsLong();
        String text = textElement.getAsString();

        if (!allowed.contains(chat)) {
            log.accept("ignored message from unauthorised chat " + chat);
            return;
        }
        for (String reply : commands.handle(chat, text)) {
            for (String part : Text.chunk(reply, OUT_MAX)) {
                try {
                    api.sendMessage(chat, part);
                } catch (IOException e) {
                    log.accept("sendMessage failed: " + e.getMessage());
                }
            }
        }
    }

    private static JsonObject parseObject(String json) throws IOException {
        try {
            JsonElement element = JsonParser.parseString(json);
            if (!element.isJsonObject()) {
                throw new IOException("unexpected response from Telegram");
            }
            return element.getAsJsonObject();
        } catch (JsonParseException e) {
            throw new IOException("bad response from Telegram");
        }
    }
}
