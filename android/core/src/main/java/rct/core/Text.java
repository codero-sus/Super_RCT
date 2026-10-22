package rct.core;

import java.util.ArrayList;
import java.util.List;

/** Splits long replies into pieces that fit one Telegram message. */
public final class Text {
    private Text() {
    }

    /** Splits at line breaks where possible; a line longer than max is cut into pieces. */
    public static List<String> chunk(String text, int max) {
        List<String> out = new ArrayList<>();
        StringBuilder current = new StringBuilder();
        for (String line : text.split("\n", -1)) {
            while (line.length() > max) {
                flush(current, out);
                out.add(line.substring(0, max));
                line = line.substring(max);
            }
            if (current.length() + line.length() + 1 > max) {
                flush(current, out);
            }
            if (current.length() > 0) {
                current.append('\n');
            }
            current.append(line);
        }
        flush(current, out);
        return out;
    }

    private static void flush(StringBuilder current, List<String> out) {
        if (current.length() > 0) {
            out.add(current.toString());
            current.setLength(0);
        }
    }
}
