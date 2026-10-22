package rct.core;

import org.junit.jupiter.api.Test;

import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

class ConfigTest {
    private static Map<String, String> good() {
        Map<String, String> m = new HashMap<>();
        m.put("SUPER_RCT_TOKEN", "123456:ABCdef_ghi-jkl1234567");
        m.put("SUPER_RCT_PASSWORD", "correct horse");
        m.put("SUPER_RCT_CHAT_IDS", "42, -100123");
        return m;
    }

    @Test
    void parsesValidSettingsWithDefaults() {
        Config c = Config.fromMap(good());
        assertEquals(Config.DEFAULT_SESSION_S, c.getSessionSeconds());
        assertEquals(new HashSet<>(Arrays.asList(42L, -100123L)), c.getChatIds());
        assertEquals("correct horse", c.getPassword());
    }

    @Test
    void readsSessionSeconds() {
        Map<String, String> m = good();
        m.put("SUPER_RCT_SESSION_SECONDS", "600");
        assertEquals(600, Config.fromMap(m).getSessionSeconds());
    }

    @Test
    void rejectsShortPassword() {
        Map<String, String> m = good();
        m.put("SUPER_RCT_PASSWORD", "short");
        assertThrows(ConfigException.class, () -> Config.fromMap(m));
    }

    @Test
    void rejectsMalformedToken() {
        Map<String, String> m = good();
        m.put("SUPER_RCT_TOKEN", "not-a-token");
        assertThrows(ConfigException.class, () -> Config.fromMap(m));
    }

    @Test
    void rejectsMissingChatIds() {
        Map<String, String> m = good();
        m.put("SUPER_RCT_CHAT_IDS", " , ");
        assertThrows(ConfigException.class, () -> Config.fromMap(m));
    }

    @Test
    void rejectsNonNumericChatId() {
        Map<String, String> m = good();
        m.put("SUPER_RCT_CHAT_IDS", "42,abc");
        assertThrows(ConfigException.class, () -> Config.fromMap(m));
    }

    @Test
    void rejectsSessionOutOfRange() {
        Map<String, String> m = good();
        m.put("SUPER_RCT_SESSION_SECONDS", "2");
        assertThrows(ConfigException.class, () -> Config.fromMap(m));
        m.put("SUPER_RCT_SESSION_SECONDS", "soon");
        assertThrows(ConfigException.class, () -> Config.fromMap(m));
    }

    @Test
    void duplicateChatIdsAreCollapsed() {
        Map<String, String> m = good();
        m.put("SUPER_RCT_CHAT_IDS", "42,42");
        assertEquals(Collections.singleton(42L), Config.fromMap(m).getChatIds());
    }
}
