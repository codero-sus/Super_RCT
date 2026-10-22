package rct.core;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.assertEquals;

class AuthTest {
    private long now = 1000;

    private Auth auth() {
        return new Auth("correct horse", 60, () -> now);
    }

    @Test
    void correctPasswordAuthenticates() {
        Auth a = auth();
        assertFalse(a.isAuthenticated(1));
        assertTrue(a.login(1, "correct horse").startsWith("Authenticated"));
        assertTrue(a.isAuthenticated(1));
    }

    @Test
    void sessionExpiresWhenIdle() {
        Auth a = auth();
        a.login(1, "correct horse");
        now += 61;
        assertFalse(a.isAuthenticated(1));
    }

    @Test
    void touchSlidesTheTimeout() {
        Auth a = auth();
        a.login(1, "correct horse");
        now += 50;
        a.touch(1);
        now += 50;
        assertTrue(a.isAuthenticated(1));
    }

    @Test
    void lockoutAfterThreeWrongPasswords() {
        Auth a = auth();
        assertEquals("Wrong password (1/3 failed attempts).", a.login(1, "wrong one"));
        assertEquals("Wrong password (2/3 failed attempts).", a.login(1, "wrong two"));
        assertEquals("Wrong password. Too many failed attempts; locked for 300 s.",
                a.login(1, "wrong three"));
        assertTrue(a.login(1, "correct horse").startsWith("Too many failed attempts. Try again in 300 s."));
        now += 301;
        assertTrue(a.login(1, "correct horse").startsWith("Authenticated"));
    }

    @Test
    void chatsAreIndependent() {
        Auth a = auth();
        a.login(1, "correct horse");
        assertFalse(a.isAuthenticated(2));
    }

    @Test
    void logoutEndsSession() {
        Auth a = auth();
        a.login(1, "correct horse");
        a.logout(1);
        assertFalse(a.isAuthenticated(1));
    }

    @Test
    void emptyArgumentAsksForPassword() {
        assertEquals("Usage: /auth <password>", auth().login(1, ""));
    }
}
