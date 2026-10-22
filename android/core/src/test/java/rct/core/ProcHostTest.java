package rct.core;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

import static org.junit.jupiter.api.Assertions.assertEquals;

class ProcHostTest {
    @Test
    void readsMemoryUptimeAndLoad(@TempDir Path dir) throws IOException {
        Path mem = dir.resolve("meminfo");
        Path up = dir.resolve("uptime");
        Path load = dir.resolve("loadavg");
        Files.write(mem, Arrays.asList("MemTotal:       1048576 kB", "MemFree:  1 kB",
                "MemAvailable:    524288 kB"));
        Files.write(up, Arrays.asList("93784.12 1000.00"));
        Files.write(load, Arrays.asList("0.52 0.58 0.59 1/345 6789"));

        ProcHost host = new ProcHost(mem, up, load);
        assertEquals("Memory: 50.0% used\nUsed: 512 MB of 1024 MB", host.memory());
        assertEquals("Up 1 d 2 h 3 m", host.uptime());
        assertEquals("Load: 0.52 0.58 0.59", host.load());
    }

    @Test
    void missingFilesReportNotAvailable(@TempDir Path dir) {
        ProcHost host = new ProcHost(dir.resolve("a"), dir.resolve("b"), dir.resolve("c"));
        assertEquals("Memory: " + Commands.NOT_AVAILABLE, host.memory());
        assertEquals("Uptime: " + Commands.NOT_AVAILABLE, host.uptime());
        assertEquals("Load: " + Commands.NOT_AVAILABLE, host.load());
    }
}
