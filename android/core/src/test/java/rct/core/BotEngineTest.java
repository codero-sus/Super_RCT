package rct.core;

import com.google.gson.Gson;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

class BotEngineTest {
    static final class FakeApi implements BotEngine.Api {
        String nextResponse;
        long lastOffset = -1;
        final List<Long> chats = new ArrayList<>();
        final List<String> sent = new ArrayList<>();

        @Override
        public String getUpdates(long offset, int timeoutSeconds) {
            lastOffset = offset;
            return nextResponse;
        }

        @Override
        public void sendMessage(long chat, String text) {
            chats.add(chat);
            sent.add(text);
        }
    }

    private long now = 1000;
    private FakeApi api;
    private List<String> logs;
    private BotEngine engine;

    @BeforeEach
    void setUp() {
        api = new FakeApi();
        logs = new ArrayList<>();
        Auth auth = new Auth("correct horse", 3600, () -> now);
        Commands commands = new Commands(auth, new FixedHost(), () -> now);
        engine = new BotEngine(api, new HashSet<>(Collections.singletonList(42L)), commands, logs::add);
    }

    private static String update(long id, long chat, String text) {
        return "{\"update_id\":" + id + ",\"message\":{\"message_id\":1,\"chat\":{\"id\":"
                + chat + ",\"type\":\"private\"},\"text\":" + new Gson().toJson(text) + "}}";
    }

    private static String ok(String... updates) {
        return "{\"ok\":true,\"result\":[" + String.join(",", updates) + "]}";
    }

    @Test
    void authorisedCommandGetsReplyAndOffsetAdvances() throws IOException {
        api.nextResponse = ok(update(10, 42, "/help"));
        assertEquals(1, engine.pollOnce(0));
        assertEquals(11, engine.offset());
        assertEquals(42L, api.chats.get(0));
        assertTrue(api.sent.get(0).contains("/commands"));
    }

    @Test
    void nextPollAsksFromTheNewOffset() throws IOException {
        api.nextResponse = ok(update(10, 42, "/help"));
        engine.pollOnce(0);
        api.nextResponse = ok();
        engine.pollOnce(0);
        assertEquals(11, api.lastOffset);
    }

    @Test
    void unauthorisedChatIsIgnoredButOffsetAdvances() throws IOException {
        api.nextResponse = ok(update(5, 7, "/help"));
        engine.pollOnce(0);
        assertTrue(api.sent.isEmpty());
        assertEquals(6, engine.offset());
        assertTrue(logs.get(0).contains("unauthorised chat 7"));
    }

    @Test
    void nonTextMessagesAreSkipped() throws IOException {
        api.nextResponse = ok("{\"update_id\":3,\"message\":{\"chat\":{\"id\":42},\"sticker\":{}}}");
        engine.pollOnce(0);
        assertTrue(api.sent.isEmpty());
        assertEquals(4, engine.offset());
    }

    @Test
    void failedResponseThrows() {
        api.nextResponse = "{\"ok\":false,\"description\":\"Unauthorized\"}";
        assertThrows(IOException.class, () -> engine.pollOnce(0));
    }

    @Test
    void garbageResponseThrows() {
        api.nextResponse = "<html>not json</html>";
        assertThrows(IOException.class, () -> engine.pollOnce(0));
    }

    @Test
    void longRepliesAreSplitIntoMessagesUnderTheLimit() throws IOException {
        String longText = "/echo " + "y".repeat(7000);
        api.nextResponse = ok(update(1, 42, "/auth correct horse"), update(2, 42, longText));
        engine.pollOnce(0);
        assertEquals(3, api.sent.size()); // auth reply, then two echo pieces
        for (String part : api.sent) {
            assertTrue(part.length() <= BotEngine.OUT_MAX);
        }
    }
}
