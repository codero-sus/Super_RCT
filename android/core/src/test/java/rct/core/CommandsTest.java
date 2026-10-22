package rct.core;

import org.junit.jupiter.api.Test;

import java.util.Collections;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

class CommandsTest {
    private long now = 1000;
    private final Auth auth = new Auth("correct horse", 3600, () -> now);
    private final Commands commands = new Commands(auth, new FixedHost(), () -> now);

    private String reply(String text) {
        return commands.handle(1, text).get(0);
    }

    @Test
    void protectedCommandsNeedLogin() {
        assertEquals(Collections.singletonList("Not authenticated. Send /auth <password> first."),
                commands.handle(1, "/mem"));
    }

    @Test
    void helpWorksWithoutLogin() {
        assertTrue(reply("/help").contains("/auth <password>"));
    }

    @Test
    void passwordMayContainSpaces() {
        assertTrue(reply("/auth correct horse").startsWith("Authenticated"));
    }

    @Test
    void statusUsesDeviceReadings() {
        reply("/auth correct horse");
        String status = reply("/status");
        assertTrue(status.contains("Memory: 50.0% used"));
        assertTrue(status.contains("Up 1 h 2 m"));
        assertTrue(status.contains("Load: 0.10 0.20 0.30"));
    }

    @Test
    void echoKeepsInternalSpaces() {
        reply("/auth correct horse");
        assertEquals("hello  world", reply("/echo hello  world"));
        assertEquals("Usage: /echo <text>", reply("/echo"));
    }

    @Test
    void nonCommandTextIsIgnored() {
        assertTrue(commands.handle(1, "hello there").isEmpty());
    }

    @Test
    void botNameSuffixIsStripped() {
        assertTrue(reply("/help@SuperRctBot").contains("commands"));
    }

    @Test
    void unknownCommandAndLogout() {
        reply("/auth correct horse");
        assertEquals("Unknown command. Send /help for the list.", reply("/nope"));
        assertEquals("Logged out.", reply("/logout"));
        assertEquals("Not authenticated. Send /auth <password> first.", reply("/mem"));
    }
}
