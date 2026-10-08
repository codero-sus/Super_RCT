/*
 * monitor.h - health metrics, threshold alerts, scheduled health reports,
 * reminders, watched paths and small utilities.
 *
 * Included by super_rct.c after its command helpers, so it shares that file's
 * static state (chat table, reply helpers, fixed-argv runner). Every command
 * here requires a login, like the rest of the bot. Nothing here runs a shell
 * or captures the screen, keyboard or camera.
 */

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define METRIC_MEM 0
#define METRIC_DISK 1
#define METRIC_TEMP 2
#define METRIC_LOAD 3
#define METRIC_COUNT 4
#define MAX_DISKS 48
#define MAX_REMINDERS 32
#define MAX_REMINDERS_PER_CHAT 10
#define MAX_REMINDER_TEXT 200
#define EVAL_INTERVAL_S 60
#define REPORT_MIN_MIN 5
#define REPORT_MAX_MIN 1440

typedef struct {
    int id;
    int64_t chat;
    time_t due;
    bool used;
    char text[MAX_REMINDER_TEXT + 4];
} reminder_t;

typedef struct {
    char mount[256];
    char fstype[64];
    double pct;
} disk_t;

typedef struct {
    double v[METRIC_COUNT];
    bool ok[METRIC_COUNT];
} metrics_t;

static reminder_t g_rem[MAX_REMINDERS];
static int g_next_rem_id = 1;
static time_t g_next_eval;

/* ------------------------------------------------------------------------- */
/* Small formatting helpers                                                  */
/* ------------------------------------------------------------------------- */

static const char *metric_name(int m) {
    static const char *const names[METRIC_COUNT] = {"mem", "disk", "temp", "load"};
    return names[m];
}

static int metric_index(const char *name) {
    for (int m = 0; m < METRIC_COUNT; m++)
        if (strcmp(metric_name(m), name) == 0)
            return m;
    return -1;
}

static void metric_text(int m, double v, char *out, size_t n) {
    switch (m) {
    case METRIC_MEM:
    case METRIC_DISK:
        snprintf(out, n, "%.1f%%", v);
        break;
    case METRIC_TEMP:
        snprintf(out, n, "%.1f C", v);
        break;
    default:
        snprintf(out, n, "%.2f per core", v);
        break;
    }
}

static bool metric_range(int m, double v) {
    switch (m) {
    case METRIC_TEMP:
        return v >= 20 && v <= 120;
    case METRIC_LOAD:
        return v >= 0.1 && v <= 100;
    default:
        return v >= 1 && v <= 100;
    }
}

static void bappendf(buf_t *b, const char *fmt, ...) {
    char line[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    buf_add(b, line, strlen(line));
}

static void reply_buf(int64_t chat, buf_t *b) {
    if (b->data)
        send_long(chat, b->data);
}

static void fmt_clock(time_t t, char *out, size_t n) {
    struct tm tmv;

    localtime_r(&t, &tmv);
    strftime(out, n, "%H:%M", &tmv);
}

static void fmt_duration(double secs, char *out, size_t n) {
    long s = (long)secs;

    snprintf(out, n, "%ldd %ldh %ldm", s / 86400, (s % 86400) / 3600, (s % 3600) / 60);
}

/* ------------------------------------------------------------------------- */
/* Readers for /proc and /sys                                                */
/* ------------------------------------------------------------------------- */

static bool read_meminfo(double *mem_pct, double *swap_pct, long *mem_total_mb,
                         long *mem_used_mb, long *swap_total_mb, long *swap_used_mb) {
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    long total = 0;
    long avail = 0;
    long stotal = 0;
    long sfree = 0;

    if (!f)
        return false;
    while (fgets(line, sizeof line, f)) {
        long v;

        if (sscanf(line, "MemTotal: %ld kB", &v) == 1)
            total = v;
        else if (sscanf(line, "MemAvailable: %ld kB", &v) == 1)
            avail = v;
        else if (sscanf(line, "SwapTotal: %ld kB", &v) == 1)
            stotal = v;
        else if (sscanf(line, "SwapFree: %ld kB", &v) == 1)
            sfree = v;
    }
    fclose(f);
    if (total <= 0)
        return false;

    *mem_total_mb = total / 1024;
    *mem_used_mb = (total - avail) / 1024;
    *mem_pct = 100.0 * (double)(total - avail) / (double)total;
    *swap_total_mb = stotal / 1024;
    *swap_used_mb = (stotal - sfree) / 1024;
    *swap_pct = stotal > 0 ? 100.0 * (double)(stotal - sfree) / (double)stotal : 0.0;
    return true;
}

static bool read_load(double *l1, double *l5, double *l15) {
    FILE *f = fopen("/proc/loadavg", "r");
    bool ok;

    if (!f)
        return false;
    ok = fscanf(f, "%lf %lf %lf", l1, l5, l15) == 3;
    fclose(f);
    return ok;
}

static long ncpus(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);

    return n > 0 ? n : 1;
}

static bool read_temp_c(double *out) {
    bool found = false;
    double best = 0;

    for (int i = 0; i < 20; i++) {
        char path[96];
        FILE *f;
        long mc;

        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", i);
        f = fopen(path, "r");
        if (!f)
            continue;
        if (fscanf(f, "%ld", &mc) == 1) {
            double c = (double)mc / 1000.0;

            if (!found || c > best)
                best = c;
            found = true;
        }
        fclose(f);
    }
    if (found)
        *out = best;
    return found;
}

static bool read_cpu(unsigned long long *busy, unsigned long long *total) {
    FILE *f = fopen("/proc/stat", "r");
    unsigned long long u;
    unsigned long long ni;
    unsigned long long sy;
    unsigned long long id;
    unsigned long long io;
    unsigned long long irq;
    unsigned long long sirq;
    unsigned long long st;
    bool ok;

    if (!f)
        return false;
    ok = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                &u, &ni, &sy, &id, &io, &irq, &sirq, &st) == 8;
    fclose(f);
    if (!ok)
        return false;
    *total = u + ni + sy + id + io + irq + sirq + st;
    *busy = *total - id - io;
    return true;
}

static bool cpu_busy_pct(int seconds, double *pct) {
    unsigned long long b1;
    unsigned long long t1;
    unsigned long long b2;
    unsigned long long t2;

    if (!read_cpu(&b1, &t1))
        return false;
    sleep((unsigned)seconds);
    if (!read_cpu(&b2, &t2) || t2 <= t1)
        return false;
    *pct = 100.0 * (double)(b2 - b1) / (double)(t2 - t1);
    return true;
}

static bool net_bytes(unsigned long long *rx, unsigned long long *tx) {
    FILE *f = fopen("/proc/net/dev", "r");
    char line[512];
    unsigned long long r = 0;
    unsigned long long t = 0;
    bool any = false;

    if (!f)
        return false;
    while (fgets(line, sizeof line, f)) {
        char *colon = strchr(line, ':');
        char name[64] = "";
        unsigned long long rb = 0;
        unsigned long long tb = 0;

        if (!colon)
            continue;
        (void)sscanf(line, " %63[^:]", name);
        if (strcmp(name, "lo") == 0)
            continue;
        if (sscanf(colon + 1, "%llu %*u %*u %*u %*u %*u %*u %*u %llu", &rb, &tb) == 2) {
            r += rb;
            t += tb;
            any = true;
        }
    }
    fclose(f);
    *rx = r;
    *tx = t;
    return any;
}

static bool read_uptime(double *secs) {
    FILE *f = fopen("/proc/uptime", "r");
    bool ok;

    if (!f)
        return false;
    ok = fscanf(f, "%lf", secs) == 1;
    fclose(f);
    return ok;
}

static int count_procs(void) {
    DIR *d = opendir("/proc");
    struct dirent *e;
    int n = 0;

    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL) {
        bool num = e->d_name[0] != '\0';

        for (const char *p = e->d_name; *p; p++) {
            if (*p < '0' || *p > '9') {
                num = false;
                break;
            }
        }
        if (num)
            n++;
    }
    closedir(d);
    return n;
}

/* Octal escapes such as \040 (space) appear in /proc/mounts. */
static void unescape_mount(char *s) {
    char *r = s;
    char *w = s;

    while (*r) {
        if (r[0] == '\\' && r[1] >= '0' && r[1] <= '7' && r[2] >= '0' && r[2] <= '7' &&
            r[3] >= '0' && r[3] <= '7') {
            *w++ = (char)((r[1] - '0') * 64 + (r[2] - '0') * 8 + (r[3] - '0'));
            r += 4;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static bool fs_is_real(const char *fs) {
    static const char *const real[] = {"ext2", "ext3", "ext4", "xfs", "btrfs", "f2fs",
                                       "vfat", "exfat", "ntfs", "ntfs3", "zfs", "jfs"};

    for (size_t i = 0; i < sizeof real / sizeof real[0]; i++)
        if (strcmp(fs, real[i]) == 0)
            return true;
    return false;
}

/* Usage of every real, mounted filesystem, like df. */
static int disk_scan(disk_t *out, int max) {
    FILE *f = fopen("/proc/mounts", "r");
    char line[1024];
    int n = 0;

    if (!f)
        return 0;
    while (n < max && fgets(line, sizeof line, f)) {
        char dev[256];
        char mnt[256];
        char fs[64];
        struct statvfs sv;
        double used;
        double avail;
        bool dup = false;

        if (sscanf(line, "%255s %255s %63s", dev, mnt, fs) != 3)
            continue;
        if (!fs_is_real(fs))
            continue;
        unescape_mount(mnt);
        for (int i = 0; i < n; i++)
            if (strcmp(out[i].mount, mnt) == 0)
                dup = true;
        if (dup)
            continue;
        if (statvfs(mnt, &sv) != 0 || sv.f_blocks == 0)
            continue;
        used = (double)(sv.f_blocks - sv.f_bfree);
        avail = (double)sv.f_bavail;
        if (used + avail <= 0)
            continue;
        snprintf(out[n].mount, sizeof out[n].mount, "%s", mnt);
        snprintf(out[n].fstype, sizeof out[n].fstype, "%s", fs);
        out[n].pct = 100.0 * used / (used + avail);
        n++;
    }
    fclose(f);
    return n;
}

/* Runs a fixed program and returns true on exit status 0. */
static bool capture_ok(char *const *argv, buf_t *out) {
    return run_argv_vec(argv, out) == 0;
}

/* Counts non-blank lines, or lines containing `needle` when it is not empty. */
static int count_lines(const buf_t *b, const char *needle) {
    int n = 0;
    const char *p = b->data ? b->data : "";

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        bool hit;

        if (needle[0] == '\0') {
            hit = false;
            for (size_t i = 0; i < len; i++)
                if (p[i] != ' ' && p[i] != '\t' && p[i] != '\r')
                    hit = true;
        } else {
            char line[512];
            size_t k = len < sizeof line - 1 ? len : sizeof line - 1;

            memcpy(line, p, k);
            line[k] = '\0';
            hit = strstr(line, needle) != NULL;
        }
        if (hit)
            n++;
        if (!nl)
            break;
        p = nl + 1;
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* Metrics, health summary and alerts                                        */
/* ------------------------------------------------------------------------- */

static void collect_metrics(metrics_t *m) {
    double mem_pct = 0;
    double swap_pct = 0;
    double l1 = 0;
    double l5 = 0;
    double l15 = 0;
    double temp = 0;
    long mt;
    long mu;
    long st;
    long su;
    disk_t disks[MAX_DISKS];
    int nd;

    memset(m, 0, sizeof *m);
    if (read_meminfo(&mem_pct, &swap_pct, &mt, &mu, &st, &su)) {
        m->v[METRIC_MEM] = mem_pct;
        m->ok[METRIC_MEM] = true;
    }
    nd = disk_scan(disks, MAX_DISKS);
    for (int i = 0; i < nd; i++) {
        if (!m->ok[METRIC_DISK] || disks[i].pct > m->v[METRIC_DISK]) {
            m->v[METRIC_DISK] = disks[i].pct;
            m->ok[METRIC_DISK] = true;
        }
    }
    if (read_temp_c(&temp)) {
        m->v[METRIC_TEMP] = temp;
        m->ok[METRIC_TEMP] = true;
    }
    if (read_load(&l1, &l5, &l15)) {
        m->v[METRIC_LOAD] = l1 / (double)ncpus();
        m->ok[METRIC_LOAD] = true;
    }
}

static void evaluate_alerts(const metrics_t *m) {
    for (int i = 0; i < g_nchats; i++) {
        chat_state *c = &g_chats[i];

        for (int k = 0; k < METRIC_COUNT; k++) {
            double hyst = k == METRIC_LOAD ? 0.2 : 5.0;
            char vt[48];
            char tt[48];

            if (c->thr[k] < 0 || !m->ok[k])
                continue;
            if (!c->fired[k] && m->v[k] >= c->thr[k]) {
                c->fired[k] = true;
                metric_text(k, m->v[k], vt, sizeof vt);
                metric_text(k, c->thr[k], tt, sizeof tt);
                reply(c->chat, "ALERT %s is %s, at or above the limit of %s",
                      metric_name(k), vt, tt);
            } else if (c->fired[k] && m->v[k] < c->thr[k] - hyst) {
                c->fired[k] = false;
                metric_text(k, m->v[k], vt, sizeof vt);
                reply(c->chat, "RECOVERED %s is back to %s", metric_name(k), vt);
            }
        }
    }
}

static void build_health(buf_t *b) {
    double mem_pct = 0;
    double swap_pct = 0;
    double l1 = 0;
    double l5 = 0;
    double l15 = 0;
    double temp = 0;
    double up = 0;
    long mt;
    long mu;
    long st;
    long su;
    disk_t disks[MAX_DISKS];
    int nd;
    int worst = -1;
    int procs;
    char dur[64];
    int failed = 0;
    int updates = 0;
    bool failed_ok = false;
    bool updates_ok = false;
    buf_t out = {0};
    char *fu[] = {"systemctl", "--failed", "--no-legend", "--plain", NULL};
    char *ua[] = {"apt", "list", "--upgradable", NULL};

    bappendf(b, "Health (Super_RCT %s)\n", SRCT_VERSION);

    if (read_meminfo(&mem_pct, &swap_pct, &mt, &mu, &st, &su)) {
        bappendf(b, "Memory: %.1f%% used (%ld/%ld MB)\n", mem_pct, mu, mt);
        if (st > 0)
            bappendf(b, "Swap: %.1f%% used (%ld/%ld MB)\n", swap_pct, su, st);
        else
            bappendf(b, "Swap: none\n");
    } else {
        bappendf(b, "Memory: n/a\n");
    }

    nd = disk_scan(disks, MAX_DISKS);
    for (int i = 0; i < nd; i++)
        if (worst < 0 || disks[i].pct > disks[worst].pct)
            worst = i;
    if (worst >= 0)
        bappendf(b, "Disk worst: %.1f%% on %s\n", disks[worst].pct, disks[worst].mount);
    else
        bappendf(b, "Disk: n/a\n");

    if (read_temp_c(&temp))
        bappendf(b, "Temperature: %.1f C (hottest zone)\n", temp);
    else
        bappendf(b, "Temperature: n/a\n");

    if (read_load(&l1, &l5, &l15))
        bappendf(b, "Load: %.2f %.2f %.2f (%.2f per core)\n", l1, l5, l15,
                 l1 / (double)ncpus());
    else
        bappendf(b, "Load: n/a\n");

    procs = count_procs();
    if (procs >= 0)
        bappendf(b, "Processes: %d\n", procs);
    if (read_uptime(&up)) {
        fmt_duration(up, dur, sizeof dur);
        bappendf(b, "Uptime: %s\n", dur);
    }

    if (capture_ok(fu, &out)) {
        failed = count_lines(&out, "");
        failed_ok = true;
    }
    free(out.data);
    out = (buf_t){0};
    if (capture_ok(ua, &out)) {
        updates = count_lines(&out, "[upgradable");
        updates_ok = true;
    }
    free(out.data);

    if (failed_ok)
        bappendf(b, "Failed units: %d\n", failed);
    else
        bappendf(b, "Failed units: n/a\n");
    if (updates_ok)
        bappendf(b, "Updates available: %d\n", updates);
    else
        bappendf(b, "Updates available: n/a\n");
    bappendf(b, "Reboot required: %s\n",
             access("/var/run/reboot-required", F_OK) == 0 ? "yes" : "no");
}

static void send_health(int64_t chat) {
    buf_t b = {0};

    build_health(&b);
    reply_buf(chat, &b);
    buf_free(&b);
}

/* ------------------------------------------------------------------------- */
/* Background work: alerts, scheduled reports and reminders                  */
/* ------------------------------------------------------------------------- */

static void monitor_tick(time_t now) {
    bool any_alert = false;

    for (int i = 0; i < g_nchats; i++)
        for (int k = 0; k < METRIC_COUNT; k++)
            if (g_chats[i].thr[k] >= 0)
                any_alert = true;
    if (any_alert && now >= g_next_eval) {
        metrics_t m;

        g_next_eval = now + EVAL_INTERVAL_S;
        collect_metrics(&m);
        evaluate_alerts(&m);
    }

    for (int i = 0; i < g_nchats; i++) {
        chat_state *c = &g_chats[i];

        if (c->report_every > 0 && now >= c->report_next) {
            c->report_next = now + c->report_every;
            send_health(c->chat);
        }
    }

    for (int i = 0; i < MAX_REMINDERS; i++) {
        if (g_rem[i].used && g_rem[i].due <= now) {
            g_rem[i].used = false;
            reply(g_rem[i].chat, "Reminder #%d: %s", g_rem[i].id, g_rem[i].text);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Command handlers                                                          */
/* ------------------------------------------------------------------------- */

static void cmd_mem(int64_t chat) {
    double mem_pct = 0;
    double swap_pct = 0;
    long mt;
    long mu;
    long st;
    long su;
    buf_t b = {0};

    if (!read_meminfo(&mem_pct, &swap_pct, &mt, &mu, &st, &su)) {
        reply(chat, "Memory information is not available on this host.");
        return;
    }
    bappendf(&b, "Memory: %.1f%% used\nUsed: %ld MB of %ld MB\n", mem_pct, mu, mt);
    reply_buf(chat, &b);
    buf_free(&b);
}

static void cmd_swap(int64_t chat) {
    double mem_pct = 0;
    double swap_pct = 0;
    long mt;
    long mu;
    long st;
    long su;
    buf_t b = {0};

    if (!read_meminfo(&mem_pct, &swap_pct, &mt, &mu, &st, &su)) {
        reply(chat, "Memory information is not available on this host.");
        return;
    }
    if (st > 0)
        bappendf(&b, "Swap: %.1f%% used\nUsed: %ld MB of %ld MB\n", swap_pct, su, st);
    else
        bappendf(&b, "Swap: none configured\n");
    reply_buf(chat, &b);
    buf_free(&b);
}

static void cmd_disk(int64_t chat) {
    disk_t disks[MAX_DISKS];
    int nd = disk_scan(disks, MAX_DISKS);
    buf_t b = {0};

    if (nd == 0) {
        reply(chat, "No real filesystems found.");
        return;
    }
    for (int i = 0; i < nd; i++)
        bappendf(&b, "%s  %.1f%%  (%s)\n", disks[i].mount, disks[i].pct, disks[i].fstype);
    reply_buf(chat, &b);
    buf_free(&b);
}

static void cmd_load(int64_t chat) {
    double l1 = 0;
    double l5 = 0;
    double l15 = 0;
    buf_t b = {0};

    if (!read_load(&l1, &l5, &l15)) {
        reply(chat, "Load information is not available on this host.");
        return;
    }
    bappendf(&b, "Load: 1 min %.2f, 5 min %.2f, 15 min %.2f\nCPUs: %ld\nPer CPU (1 min): %.2f\n",
             l1, l5, l15, ncpus(), l1 / (double)ncpus());
    reply_buf(chat, &b);
    buf_free(&b);
}

static void cmd_temp(int64_t chat) {
    double t = 0;

    if (read_temp_c(&t))
        reply(chat, "Hottest thermal zone: %.1f C", t);
    else
        reply(chat, "No thermal sensors found on this host.");
}

static void cmd_cpu(int64_t chat) {
    double pct = 0;

    if (cpu_busy_pct(1, &pct))
        reply(chat, "CPU busy: %.1f%% (sampled over 1 s)", pct);
    else
        reply(chat, "CPU statistics are not available on this host.");
}

static void cmd_net_rate(int64_t chat) {
    unsigned long long r1 = 0;
    unsigned long long t1 = 0;
    unsigned long long r2 = 0;
    unsigned long long t2 = 0;

    if (!net_bytes(&r1, &t1)) {
        reply(chat, "Network statistics are not available on this host.");
        return;
    }
    sleep(1);
    if (!net_bytes(&r2, &t2)) {
        reply(chat, "Network statistics are not available on this host.");
        return;
    }
    reply(chat, "Network (excluding lo), 1 s sample:\nReceive: %.1f KB/s\nTransmit: %.1f KB/s",
          (double)(r2 - r1) / 1024.0, (double)(t2 - t1) / 1024.0);
}

static void cmd_procs(int64_t chat) {
    int n = count_procs();

    if (n < 0)
        reply(chat, "Process list is not available on this host.");
    else
        reply(chat, "Running processes: %d", n);
}

static void cmd_uptime_h(int64_t chat) {
    double up = 0;
    char dur[64];

    if (!read_uptime(&up)) {
        reply(chat, "Uptime is not available on this host.");
        return;
    }
    fmt_duration(up, dur, sizeof dur);
    reply(chat, "Up %s", dur);
}

static void cmd_failed_units(int64_t chat) {
    char *argv[] = {"systemctl", "--failed", "--no-legend", "--plain", NULL};
    buf_t out = {0};
    int status = run_argv_vec(argv, &out);
    buf_t b = {0};

    if (status != 0) {
        reply(chat, "systemd is not available on this host.");
        buf_free(&out);
        return;
    }
    bappendf(&b, "Failed units: %d\n", count_lines(&out, ""));
    if (out.data && out.len > 0)
        buf_add(&b, out.data, out.len);
    reply_buf(chat, &b);
    buf_free(&out);
    buf_free(&b);
}

static void cmd_updates_count(int64_t chat) {
    char *argv[] = {"apt", "list", "--upgradable", NULL};
    buf_t out = {0};

    if (!capture_ok(argv, &out))
        reply(chat, "Package updates are not available on this host (apt).");
    else
        reply(chat, "Packages with updates available: %d", count_lines(&out, "[upgradable"));
    buf_free(&out);
}

static void cmd_reboot_required(int64_t chat) {
    reply(chat, "Reboot required: %s",
          access("/var/run/reboot-required", F_OK) == 0 ? "yes" : "no");
}

static void cmd_alert(int64_t chat, chat_state *c, const char *arg) {
    char sub[16] = "";
    char name[16] = "";
    char val[32] = "";
    int m;

    (void)sscanf(arg, "%15s %15s %31s", sub, name, val);

    if (sub[0] == '\0' || strcmp(sub, "list") == 0) {
        buf_t b = {0};

        bappendf(&b, "Alert limits (reply lists current settings):\n");
        for (int k = 0; k < METRIC_COUNT; k++) {
            if (c->thr[k] < 0) {
                bappendf(&b, "%s: off\n", metric_name(k));
            } else {
                char tt[48];

                metric_text(k, c->thr[k], tt, sizeof tt);
                bappendf(&b, "%s: %s\n", metric_name(k), tt);
            }
        }
        reply_buf(chat, &b);
        buf_free(&b);
        return;
    }

    if (strcmp(sub, "set") == 0) {
        char *end = NULL;
        double v;

        m = metric_index(name);
        if (m < 0 || val[0] == '\0') {
            reply(chat, "Usage: /alert set mem|disk|temp|load <value>");
            return;
        }
        v = strtod(val, &end);
        if (end == val || *end != '\0' || !metric_range(m, v)) {
            reply(chat, "Value for %s is out of range.", name);
            return;
        }
        c->thr[m] = v;
        c->fired[m] = false;
        reply(chat, "Alert for %s set; you will be notified when it is reached.", name);
        return;
    }

    if (strcmp(sub, "off") == 0) {
        m = metric_index(name);
        if (m < 0) {
            reply(chat, "Usage: /alert off mem|disk|temp|load");
            return;
        }
        c->thr[m] = -1;
        c->fired[m] = false;
        reply(chat, "Alert for %s turned off.", name);
        return;
    }

    if (strcmp(sub, "check") == 0) {
        metrics_t mx;
        buf_t b = {0};

        collect_metrics(&mx);
        bappendf(&b, "Current values:\n");
        for (int k = 0; k < METRIC_COUNT; k++) {
            char vt[48];

            if (!mx.ok[k]) {
                bappendf(&b, "%s: n/a\n", metric_name(k));
                continue;
            }
            metric_text(k, mx.v[k], vt, sizeof vt);
            bappendf(&b, "%s: %s", metric_name(k), vt);
            if (c->thr[k] >= 0) {
                char tt[48];

                metric_text(k, c->thr[k], tt, sizeof tt);
                bappendf(&b, " (limit %s)", tt);
            }
            bappendf(&b, "\n");
        }
        reply_buf(chat, &b);
        buf_free(&b);
        return;
    }

    reply(chat, "Usage: /alert list | set <metric> <value> | off <metric> | check");
}

static void cmd_report(int64_t chat, chat_state *c, const char *arg, time_t now) {
    char sub[16] = "";
    char val[16] = "";
    char clock[8];

    (void)sscanf(arg, "%15s %15s", sub, val);

    if (sub[0] == '\0' || strcmp(sub, "status") == 0) {
        if (c->report_every <= 0) {
            reply(chat, "Scheduled reports are off. Use /report on <minutes>.");
        } else {
            fmt_clock(c->report_next, clock, sizeof clock);
            reply(chat, "Health reports every %ld minutes; next at %s.",
                  (long)(c->report_every / 60), clock);
        }
        return;
    }
    if (strcmp(sub, "on") == 0) {
        char *end = NULL;
        long mins = strtol(val, &end, 10);

        if (end == val || *end != '\0' || mins < REPORT_MIN_MIN || mins > REPORT_MAX_MIN) {
            reply(chat, "Usage: /report on <minutes>  (%d-%d)", REPORT_MIN_MIN, REPORT_MAX_MIN);
            return;
        }
        c->report_every = (time_t)mins * 60;
        c->report_next = now + c->report_every;
        reply(chat, "Health reports on: every %ld minutes.", mins);
        return;
    }
    if (strcmp(sub, "off") == 0) {
        c->report_every = 0;
        reply(chat, "Scheduled reports turned off.");
        return;
    }
    if (strcmp(sub, "now") == 0) {
        send_health(chat);
        return;
    }
    reply(chat, "Usage: /report status | on <minutes> | off | now");
}

static void cmd_remind(int64_t chat, const char *arg, time_t now) {
    char *end = NULL;
    long mins = strtol(arg, &end, 10);
    const char *text = end;
    int mine = 0;
    int slot = -1;
    char clock[8];

    if (end == arg || mins < 1 || mins > 10080) {
        reply(chat, "Usage: /remind <minutes 1-10080> <text>");
        return;
    }
    while (*text == ' ' || *text == '\t')
        text++;
    if (*text == '\0') {
        reply(chat, "Usage: /remind <minutes 1-10080> <text>");
        return;
    }
    if (strlen(text) > MAX_REMINDER_TEXT) {
        reply(chat, "Reminder text is too long (max %d characters).", MAX_REMINDER_TEXT);
        return;
    }
    for (int i = 0; i < MAX_REMINDERS; i++) {
        if (g_rem[i].used && g_rem[i].chat == chat)
            mine++;
        if (!g_rem[i].used && slot < 0)
            slot = i;
    }
    if (mine >= MAX_REMINDERS_PER_CHAT || slot < 0) {
        reply(chat, "You have too many reminders pending (max %d).", MAX_REMINDERS_PER_CHAT);
        return;
    }
    g_rem[slot].used = true;
    g_rem[slot].id = g_next_rem_id++;
    g_rem[slot].chat = chat;
    g_rem[slot].due = now + (time_t)mins * 60;
    snprintf(g_rem[slot].text, sizeof g_rem[slot].text, "%s", text);
    fmt_clock(g_rem[slot].due, clock, sizeof clock);
    reply(chat, "Reminder #%d set for %s.", g_rem[slot].id, clock);
}

static void cmd_timers(int64_t chat) {
    buf_t b = {0};
    char clock[8];

    for (int i = 0; i < MAX_REMINDERS; i++) {
        if (!g_rem[i].used || g_rem[i].chat != chat)
            continue;
        fmt_clock(g_rem[i].due, clock, sizeof clock);
        bappendf(&b, "#%d at %s: %s\n", g_rem[i].id, clock, g_rem[i].text);
    }
    if (b.len == 0)
        reply(chat, "No pending reminders.");
    else
        reply_buf(chat, &b);
    buf_free(&b);
}

static void cmd_timer_cancel(int64_t chat, const char *arg) {
    char *end = NULL;
    long id = strtol(arg, &end, 10);

    if (end == arg || *end != '\0') {
        reply(chat, "Usage: /timer_cancel <id>");
        return;
    }
    for (int i = 0; i < MAX_REMINDERS; i++) {
        if (g_rem[i].used && g_rem[i].chat == chat && g_rem[i].id == id) {
            g_rem[i].used = false;
            reply(chat, "Reminder #%ld cancelled.", id);
            return;
        }
    }
    reply(chat, "No pending reminder with id %ld.", id);
}

static void watch_line(buf_t *b, const char *path, time_t now, bool stale_only) {
    struct stat st;
    double age;

    if (stat(path, &st) != 0) {
        bappendf(b, "%s  (missing)\n", path);
        return;
    }
    age = difftime(now, st.st_mtime) / 3600.0;
    if (stale_only && age <= (double)g_watch_max_h)
        return;
    bappendf(b, "%s  age %.1f h  %lld bytes%s\n", path, age, (long long)st.st_size,
             age > (double)g_watch_max_h ? "  STALE" : "");
}

static void cmd_watch(int64_t chat, bool stale_only, time_t now) {
    buf_t b = {0};

    if (g_nwatch == 0) {
        reply(chat, "No watch paths configured (set SUPER_RCT_WATCH_PATHS).");
        return;
    }
    for (int i = 0; i < g_nwatch; i++)
        watch_line(&b, g_watch[i], now, stale_only);
    if (stale_only && b.len == 0)
        bappendf(&b, "All %d watched paths are fresh (limit %ld h).", g_nwatch, g_watch_max_h);
    reply_buf(chat, &b);
    buf_free(&b);
}

static void cmd_notify(int64_t chat, const char *arg) {
    char clean[MAX_REMINDER_TEXT + 1];
    size_t n = 0;
    char *argv[] = {"notify-send", "Super_RCT", clean, NULL};
    int status;
    buf_t out = {0};

    if (arg[0] == '\0') {
        reply(chat, "Usage: /notify <text>");
        return;
    }
    for (const char *p = arg; *p && n < MAX_REMINDER_TEXT; p++)
        clean[n++] = ((unsigned char)*p < 0x20) ? ' ' : *p;
    clean[n] = '\0';

    status = run_argv_vec(argv, &out);
    buf_free(&out);
    if (status == 0)
        reply(chat, "Notification sent to the host.");
    else if (status == 127)
        reply(chat, "notify-send is not installed on this host.");
    else
        reply(chat, "notify-send failed (exit %d).", status);
}

static void cmd_botstats(int64_t chat, time_t now) {
    int active = 0;
    int pending = 0;
    int alerts = 0;

    for (int i = 0; i < g_nchats; i++) {
        if (g_chats[i].session_until > now)
            active++;
        for (int k = 0; k < METRIC_COUNT; k++)
            if (g_chats[i].thr[k] >= 0) {
                alerts++;
                break;
            }
    }
    for (int i = 0; i < MAX_REMINDERS; i++)
        if (g_rem[i].used)
            pending++;
    reply(chat,
          "Uptime: %ld s\nCommands handled: %lu\nFailed logins: %lu\n"
          "Active sessions: %d of %d\nChats with alerts: %d\n"
          "Reminders pending: %d\nWatched paths: %d",
          (long)difftime(now, g_started), g_cmds_handled, g_auth_failures, active,
          g_nchats, alerts, pending, g_nwatch);
}

static void cmd_sessions(int64_t chat, time_t now) {
    buf_t b = {0};

    for (int i = 0; i < g_nchats; i++) {
        if (g_chats[i].session_until > now)
            bappendf(&b, "chat %lld: logged in, %ld s left\n", (long long)g_chats[i].chat,
                     (long)(g_chats[i].session_until - now));
        else
            bappendf(&b, "chat %lld: not logged in\n", (long long)g_chats[i].chat);
    }
    reply_buf(chat, &b);
    buf_free(&b);
}

static void cmd_logout_all(int64_t chat) {
    for (int i = 0; i < g_nchats; i++)
        g_chats[i].session_until = 0;
    reply(chat, "All login sessions ended. Send /auth <password> to log in again.");
}

static void cmd_selftest(int64_t chat) {
    buf_t b = {0};
    char dir[MAX_URL];
    char *slash = strrchr(g_ipfile, '/');
    char *na[] = {"notify-send", "--version", NULL};
    char *sa[] = {"systemctl", "--version", NULL};
    buf_t tmp = {0};

    bappendf(&b, "Config: OK\n");
    bappendf(&b, "/proc readable: %s\n", access("/proc/meminfo", R_OK) == 0 ? "PASS" : "FAIL");
    {
        struct statvfs sv;
        bappendf(&b, "Disk statistics: %s\n", statvfs("/", &sv) == 0 ? "PASS" : "FAIL");
    }
    if (slash) {
        size_t n = (size_t)(slash - g_ipfile);
        if (n == 0)
            snprintf(dir, sizeof dir, "/");
        else if (n < sizeof dir) {
            memcpy(dir, g_ipfile, n);
            dir[n] = '\0';
        } else
            snprintf(dir, sizeof dir, ".");
    } else {
        snprintf(dir, sizeof dir, ".");
    }
    bappendf(&b, "IP registry directory writable: %s\n",
             access(dir, W_OK) == 0 ? "PASS" : "FAIL");
    bappendf(&b, "notify-send: %s\n",
             run_argv_vec(na, &tmp) == 127 ? "not installed" : "installed");
    buf_free(&tmp);
    bappendf(&b, "systemctl: %s\n",
             run_argv_vec(sa, &tmp) == 127 ? "not installed" : "installed");
    buf_free(&tmp);
    bappendf(&b, "Commands loaded: %d\n", RCT_NCOMMANDS);
    reply_buf(chat, &b);
    buf_free(&b);
}

static void cmd_config(int64_t chat) {
    const char *host = strstr(g_api, "://");
    const char *end;
    buf_t b = {0};
    int alerts = 0;

    for (int i = 0; i < g_nchats; i++)
        for (int k = 0; k < METRIC_COUNT; k++)
            if (g_chats[i].thr[k] >= 0) {
                alerts++;
                break;
            }
    host = host ? host + 3 : g_api;
    end = strchr(host, '/');
    bappendf(&b, "Allowed chats: %d\n", g_nchats);
    bappendf(&b, "Session timeout: %ld s\n", g_session_s);
    bappendf(&b, "API host: %.*s\n", end ? (int)(end - host) : (int)strlen(host), host);
    bappendf(&b, "IP registry file: %s\n", g_ipfile);
    bappendf(&b, "Watch paths: %d (max age %ld h)\n", g_nwatch, g_watch_max_h);
    bappendf(&b, "Chats with alerts: %d\n", alerts);
    bappendf(&b, "Token and password: not shown\n");
    reply_buf(chat, &b);
    buf_free(&b);
}

static void cmd_about(int64_t chat, time_t now) {
    char dur[64];

    fmt_duration(difftime(now, g_started), dur, sizeof dur);
    reply(chat, "Super_RCT %s (variant: C)\nUptime: %s\nCommands: %d",
          SRCT_VERSION, dur, RCT_NCOMMANDS);
}

static void cmd_echo(int64_t chat, const char *arg) {
    if (arg[0] == '\0')
        reply(chat, "Usage: /echo <text>");
    else
        reply(chat, "%s", arg);
}

/* Returns true if `cmd` was one of this module's commands. */
static bool monitor_dispatch(int64_t chat, chat_state *c, const char *cmd, const char *arg,
                             time_t now) {
    if (strcmp(cmd, "/health") == 0)
        send_health(chat);
    else if (strcmp(cmd, "/mem") == 0)
        cmd_mem(chat);
    else if (strcmp(cmd, "/swap") == 0)
        cmd_swap(chat);
    else if (strcmp(cmd, "/disk") == 0)
        cmd_disk(chat);
    else if (strcmp(cmd, "/load") == 0)
        cmd_load(chat);
    else if (strcmp(cmd, "/temp") == 0)
        cmd_temp(chat);
    else if (strcmp(cmd, "/cpu") == 0)
        cmd_cpu(chat);
    else if (strcmp(cmd, "/net_rate") == 0)
        cmd_net_rate(chat);
    else if (strcmp(cmd, "/procs") == 0)
        cmd_procs(chat);
    else if (strcmp(cmd, "/uptime_h") == 0)
        cmd_uptime_h(chat);
    else if (strcmp(cmd, "/failed_units") == 0)
        cmd_failed_units(chat);
    else if (strcmp(cmd, "/updates_count") == 0)
        cmd_updates_count(chat);
    else if (strcmp(cmd, "/reboot_required") == 0)
        cmd_reboot_required(chat);
    else if (strcmp(cmd, "/alert") == 0)
        cmd_alert(chat, c, arg);
    else if (strcmp(cmd, "/report") == 0)
        cmd_report(chat, c, arg, now);
    else if (strcmp(cmd, "/remind") == 0)
        cmd_remind(chat, arg, now);
    else if (strcmp(cmd, "/timers") == 0)
        cmd_timers(chat);
    else if (strcmp(cmd, "/timer_cancel") == 0)
        cmd_timer_cancel(chat, arg);
    else if (strcmp(cmd, "/watch") == 0)
        cmd_watch(chat, false, now);
    else if (strcmp(cmd, "/watch_check") == 0)
        cmd_watch(chat, true, now);
    else if (strcmp(cmd, "/notify") == 0)
        cmd_notify(chat, arg);
    else if (strcmp(cmd, "/echo") == 0)
        cmd_echo(chat, arg);
    else if (strcmp(cmd, "/botstats") == 0)
        cmd_botstats(chat, now);
    else if (strcmp(cmd, "/sessions") == 0)
        cmd_sessions(chat, now);
    else if (strcmp(cmd, "/logout_all") == 0)
        cmd_logout_all(chat);
    else if (strcmp(cmd, "/about") == 0)
        cmd_about(chat, now);
    else if (strcmp(cmd, "/selftest") == 0)
        cmd_selftest(chat);
    else if (strcmp(cmd, "/config") == 0)
        cmd_config(chat);
    else
        return false;
    return true;
}
