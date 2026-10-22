package rct.core;

/** A device host with fixed readings, for tests. */
final class FixedHost implements Commands.Host {
    @Override
    public String memory() {
        return "Memory: 50.0% used";
    }

    @Override
    public String uptime() {
        return "Up 1 h 2 m";
    }

    @Override
    public String load() {
        return "Load: 0.10 0.20 0.30";
    }
}
