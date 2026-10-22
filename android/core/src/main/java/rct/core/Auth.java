package rct.core;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.HashMap;
import java.util.Map;
import java.util.function.LongSupplier;

/**
 * Login sessions per chat: password check, idle timeout, and a lockout after
 * repeated wrong passwords. Same rules as the other editions.
 */
public final class Auth {
    public static final int MAX_FAILS = 3;
    public static final long LOCKOUT_S = 300;

    private static final class Session {
        long sessionUntil;
        int fails;
        long lockedUntil;
    }

    private final byte[] password;
    private final long sessionSeconds;
    private final LongSupplier clock; // seconds since the epoch
    private final Map<Long, Session> sessions = new HashMap<>();

    public Auth(String password, long sessionSeconds, LongSupplier clock) {
        this.password = password.getBytes(StandardCharsets.UTF_8);
        this.sessionSeconds = sessionSeconds;
        this.clock = clock;
    }

    private Session session(long chat) {
        return sessions.computeIfAbsent(chat, c -> new Session());
    }

    /** Handles /auth <password>. Returns the reply to send. */
    public synchronized String login(long chat, String arg) {
        long now = clock.getAsLong();
        Session s = session(chat);
        if (s.lockedUntil > now) {
            return "Too many failed attempts. Try again in " + (s.lockedUntil - now) + " s.";
        }
        if (arg.isEmpty()) {
            return "Usage: /auth <password>";
        }
        if (MessageDigest.isEqual(password, arg.getBytes(StandardCharsets.UTF_8))) {
            s.fails = 0;
            s.sessionUntil = now + sessionSeconds;
            return "Authenticated. Session expires after " + sessionSeconds + " s of inactivity.";
        }
        s.fails++;
        if (s.fails >= MAX_FAILS) {
            s.fails = 0;
            s.sessionUntil = 0;
            s.lockedUntil = now + LOCKOUT_S;
            return "Wrong password. Too many failed attempts; locked for " + LOCKOUT_S + " s.";
        }
        return "Wrong password (" + s.fails + "/" + MAX_FAILS + " failed attempts).";
    }

    public synchronized void logout(long chat) {
        session(chat).sessionUntil = 0;
    }

    public synchronized boolean isAuthenticated(long chat) {
        return session(chat).sessionUntil > clock.getAsLong();
    }

    /** Slides the idle timeout forward. Call only after isAuthenticated returned true. */
    public synchronized void touch(long chat) {
        session(chat).sessionUntil = clock.getAsLong() + sessionSeconds;
    }
}
