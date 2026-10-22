package rct.core;

/** Thrown when settings are missing or invalid. The message is safe to show to the user. */
public final class ConfigException extends RuntimeException {
    public ConfigException(String message) {
        super(message);
    }
}
