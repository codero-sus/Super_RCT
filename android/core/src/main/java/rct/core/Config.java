package rct.core;

import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;
import java.util.regex.Pattern;

/**
 * Settings for one bot run. The variable names and limits match the C and
 * Python editions (SUPER_RCT_* in python.env.example).
 */
public final class Config {
    public static final int MIN_PASSWORD = 8;
    public static final int MAX_PASSWORD = 128;
    public static final int MIN_SESSION_S = 5;
    public static final int MAX_SESSION_S = 86400;
    public static final int DEFAULT_SESSION_S = 3600;
    public static final int MAX_CHATS = 32;

    private static final Pattern TOKEN_RE = Pattern.compile("[0-9]{1,20}:[A-Za-z0-9_-]{10,200}");

    private final String token;
    private final String password;
    private final int sessionSeconds;
    private final Set<Long> chatIds;

    private Config(String token, String password, int sessionSeconds, Set<Long> chatIds) {
        this.token = token;
        this.password = password;
        this.sessionSeconds = sessionSeconds;
        this.chatIds = chatIds;
    }

    /** Builds and checks the settings. Throws ConfigException with a readable reason. */
    public static Config fromMap(Map<String, String> env) {
        String token = value(env, "SUPER_RCT_TOKEN").trim();
        if (!TOKEN_RE.matcher(token).matches()) {
            throw new ConfigException("SUPER_RCT_TOKEN is missing or malformed");
        }

        String password = value(env, "SUPER_RCT_PASSWORD");
        int bytes = password.getBytes(StandardCharsets.UTF_8).length;
        if (bytes < MIN_PASSWORD || bytes > MAX_PASSWORD) {
            throw new ConfigException("SUPER_RCT_PASSWORD is required ("
                    + MIN_PASSWORD + "-" + MAX_PASSWORD + " bytes)");
        }

        int session = DEFAULT_SESSION_S;
        String rawSession = value(env, "SUPER_RCT_SESSION_SECONDS").trim();
        if (!rawSession.isEmpty()) {
            String range = "SUPER_RCT_SESSION_SECONDS must be "
                    + MIN_SESSION_S + "-" + MAX_SESSION_S;
            try {
                session = Integer.parseInt(rawSession);
            } catch (NumberFormatException e) {
                throw new ConfigException(range);
            }
            if (session < MIN_SESSION_S || session > MAX_SESSION_S) {
                throw new ConfigException(range);
            }
        }

        Set<Long> chats = new LinkedHashSet<>();
        for (String part : value(env, "SUPER_RCT_CHAT_IDS").split(",")) {
            String id = part.trim();
            if (id.isEmpty()) {
                continue;
            }
            try {
                chats.add(Long.parseLong(id));
            } catch (NumberFormatException e) {
                throw new ConfigException("bad chat id list in SUPER_RCT_CHAT_IDS");
            }
        }
        if (chats.isEmpty()) {
            throw new ConfigException("SUPER_RCT_CHAT_IDS is required (comma-separated)");
        }
        if (chats.size() > MAX_CHATS) {
            throw new ConfigException("SUPER_RCT_CHAT_IDS allows at most " + MAX_CHATS + " chats");
        }
        return new Config(token, password, session, Collections.unmodifiableSet(chats));
    }

    private static String value(Map<String, String> env, String key) {
        String v = env.get(key);
        return v == null ? "" : v;
    }

    public String getToken() {
        return token;
    }

    public String getPassword() {
        return password;
    }

    public int getSessionSeconds() {
        return sessionSeconds;
    }

    public Set<Long> getChatIds() {
        return chatIds;
    }
}
