package rct.core;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.List;
import java.util.Locale;

/**
 * Reads memory, uptime and load from /proc files. Android may hide some of these
 * from apps; a missing or unreadable file gives the NOT_AVAILABLE text instead.
 */
public final class ProcHost implements Commands.Host {
    private final Path meminfo;
    private final Path uptime;
    private final Path loadavg;

    public ProcHost(Path meminfo, Path uptime, Path loadavg) {
        this.meminfo = meminfo;
        this.uptime = uptime;
        this.loadavg = loadavg;
    }

    /** The real /proc files on the device. */
    public static ProcHost device() {
        return new ProcHost(Paths.get("/proc/meminfo"), Paths.get("/proc/uptime"),
                Paths.get("/proc/loadavg"));
    }

    @Override
    public String memory() {
        try {
            long total = -1;
            long available = -1;
            for (String line : Files.readAllLines(meminfo, StandardCharsets.UTF_8)) {
                if (line.startsWith("MemTotal:")) {
                    total = kib(line);
                } else if (line.startsWith("MemAvailable:")) {
                    available = kib(line);
                }
            }
            if (total <= 0 || available < 0) {
                return "Memory: " + Commands.NOT_AVAILABLE;
            }
            long used = total - available;
            return String.format(Locale.ROOT, "Memory: %.1f%% used\nUsed: %d MB of %d MB",
                    100.0 * used / total, used / 1024, total / 1024);
        } catch (IOException | RuntimeException e) {
            return "Memory: " + Commands.NOT_AVAILABLE;
        }
    }

    @Override
    public String uptime() {
        try {
            String first = Files.readAllLines(uptime, StandardCharsets.UTF_8).get(0).trim().split("\\s+")[0];
            long secs = (long) Double.parseDouble(first);
            long days = secs / 86400;
            long hours = (secs % 86400) / 3600;
            long minutes = (secs % 3600) / 60;
            if (days > 0) {
                return String.format(Locale.ROOT, "Up %d d %d h %d m", days, hours, minutes);
            }
            if (hours > 0) {
                return String.format(Locale.ROOT, "Up %d h %d m", hours, minutes);
            }
            return String.format(Locale.ROOT, "Up %d m %d s", minutes, secs % 60);
        } catch (IOException | RuntimeException e) {
            return "Uptime: " + Commands.NOT_AVAILABLE;
        }
    }

    @Override
    public String load() {
        try {
            String[] parts = Files.readAllLines(loadavg, StandardCharsets.UTF_8).get(0).trim().split("\\s+");
            if (parts.length < 3) {
                return "Load: " + Commands.NOT_AVAILABLE;
            }
            return "Load: " + parts[0] + " " + parts[1] + " " + parts[2];
        } catch (IOException | RuntimeException e) {
            return "Load: " + Commands.NOT_AVAILABLE;
        }
    }

    /** Parses a meminfo line such as "MemTotal:  1048576 kB" into kibibytes. */
    private static long kib(String line) {
        List<String> parts = java.util.Arrays.asList(line.trim().split("\\s+"));
        return Long.parseLong(parts.get(1));
    }
}
