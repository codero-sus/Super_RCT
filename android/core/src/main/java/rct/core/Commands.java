package rct.core;

import java.util.Collections;
import java.util.List;
import java.util.function.LongSupplier;

/**
 * Command handling for the Android edition. Pure Java: no network, no Android
 * APIs, so every reply can be tested. The Android edition has a smaller command
 * set than the C and Python editions (see android/README.md).
 */
public final class Commands {
    public static final String VERSION = "0.5.0";
    public static final String NOT_AVAILABLE = "not available on this device";

    /** Device readings. Each returns a ready-to-send line, or NOT_AVAILABLE text. */
    public interface Host {
        String memory();

        String uptime();

        String load();
    }

    static final String HELP = "Super_RCT " + VERSION + " (Android edition) commands:\n"
            + "/auth <password> - log in (required before other commands)\n"
            + "/logout - end your session\n"
            + "/status - app and device status\n"
            + "/mem - memory use\n"
            + "/uptime - device uptime\n"
            + "/load - load average\n"
            + "/echo <text> - repeat the text\n"
            + "/commands - list commands\n"
            + "/help - this message";

    static final String COMMANDS = "/auth /logout /status /mem /uptime /load /echo /commands /help";

    private final Auth auth;
    private final Host host;
    private final LongSupplier clock;
    private final long startedAt;

    public Commands(Auth auth, Host host, LongSupplier clock) {
        this.auth = auth;
        this.host = host;
        this.clock = clock;
        this.startedAt = clock.getAsLong();
    }

    /** Returns the replies for one incoming message. Empty when the text is not a command. */
    public List<String> handle(long chat, String text) {
        // The first word is the command; the rest (internal spaces kept) is the argument,
        // so passwords may contain spaces.
        String stripped = text.replaceFirst("^[ \\t]+", "");
        int end = 0;
        while (end < stripped.length() && stripped.charAt(end) != ' ' && stripped.charAt(end) != '\t') {
            end++;
        }
        String cmd = stripped.substring(0, end);
        String arg = stripped.substring(end).replaceAll("^[ \\t\\r\\n]+|[ \\t\\r\\n]+$", "");
        if (!cmd.startsWith("/")) {
            return Collections.emptyList();
        }
        cmd = cmd.split("@", 2)[0]; // groups send /cmd@BotName

        switch (cmd) {
            case "/start":
            case "/help":
                return one(HELP);
            case "/auth":
                return one(auth.login(chat, arg));
            case "/logout":
                auth.logout(chat);
                return one("Logged out.");
            default:
                break;
        }

        if (!auth.isAuthenticated(chat)) {
            return one("Not authenticated. Send /auth <password> first.");
        }
        auth.touch(chat);

        switch (cmd) {
            case "/status":
                return one("Super_RCT " + VERSION + " (Android)\n"
                        + "App running for " + (clock.getAsLong() - startedAt) + " s\n"
                        + host.uptime() + "\n"
                        + host.memory() + "\n"
                        + host.load());
            case "/mem":
                return one(host.memory());
            case "/uptime":
                return one(host.uptime());
            case "/load":
                return one(host.load());
            case "/echo":
                return one(arg.isEmpty() ? "Usage: /echo <text>" : arg);
            case "/commands":
                return one(COMMANDS);
            default:
                return one("Unknown command. Send /help for the list.");
        }
    }

    private static List<String> one(String reply) {
        return Collections.singletonList(reply);
    }
}
