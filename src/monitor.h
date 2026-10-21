/*
 * monitor.h - health metrics, threshold alerts, scheduled health reports,
 * reminders, watched paths and small utilities.
 *
 * Included by super_rct.c after its command helpers, so it shares that file's
 * static state (chat table, reply helpers, fixed-argv runner). Every command
 * here requires a login, like the rest of the bot. Nothing here runs a shell
 * or captures the screen, keyboard or camera.
 */

#include <sys/stat.h>
#ifdef _WIN32
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <winreg.h>
#else
#include <dirent.h>
#include <sys/statvfs.h>
#endif

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
#define COOLDOWN_MAX_MIN 1440
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

    plat_localtime(t, &tmv);
    strftime(out, n, "%H:%M", &tmv);
}

static void fmt_duration(double secs, char *out, size_t n) {
    long s = (long)secs;

    snprintf(out, n, "%ldd %ldh %ldm", s / 86400, (s % 86400) / 3600, (s % 3600) / 60);
}

/* ------------------------------------------------------------------------- */
/* Helpers shared by both platforms. */
#ifdef _WIN32
static bool host_stats_readable(void) {
    MEMORYSTATUSEX ms;

    memset(&ms, 0, sizeof ms);
    ms.dwLength = sizeof ms;
    return GlobalMemoryStatusEx(&ms) != 0;
}

static bool disk_stats_ok(void) {
    ULARGE_INTEGER avail;
    ULARGE_INTEGER total;
    ULARGE_INTEGER freeb;

    return GetDiskFreeSpaceExA("C:\\", &avail, &total, &freeb) != 0;
}

static bool reboot_required(void) {
    HKEY key;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired",
                      0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    RegCloseKey(key);
    return true;
}
#else
static bool host_stats_readable(void) {
    return access("/proc/meminfo", R_OK) == 0;
}

static bool disk_stats_ok(void) {
    struct statvfs sv;

    return statvfs("/", &sv) == 0;
}

static bool reboot_required(void) {
    return access("/var/run/reboot-required", F_OK) == 0;
}
#endif

/* Readers for /proc and /sys                                                */
/* ------------------------------------------------------------------------- */

#ifdef _WIN32

static unsigned long long win_filetime(const FILETIME *ft) {
    return ((unsigned long long)ft->dwHighDateTime << 32) | (unsigned long long)ft->dwLowDateTime;
}

static bool read_meminfo(double *mem_pct, double *swap_pct, long *mem_total_mb,
                         long *mem_used_mb, long *swap_total_mb, long *swap_used_mb) {
    MEMORYSTATUSEX ms;
    unsigned long long phys_used;
    unsigned long long swap_total;
    unsigned long long swap_used;

    memset(&ms, 0, sizeof ms);
    ms.dwLength = sizeof ms;
    if (!GlobalMemoryStatusEx(&ms) || ms.ullTotalPhys == 0)
        return false;
    phys_used = ms.ullTotalPhys - ms.ullAvailPhys;
    *mem_total_mb = (long)(ms.ullTotalPhys / 1048576ULL);
    *mem_used_mb = (long)(phys_used / 1048576ULL);
    *mem_pct = 100.0 * (double)phys_used / (double)ms.ullTotalPhys;
    /* The page file total includes physical memory, so swap is the difference. */
    swap_total = ms.ullTotalPageFile > ms.ullTotalPhys ? ms.ullTotalPageFile - ms.ullTotalPhys : 0;
    swap_used = (ms.ullTotalPageFile - ms.ullAvailPageFile) > phys_used
                    ? (ms.ullTotalPageFile - ms.ullAvailPageFile) - phys_used
                    : 0;
    if (swap_used > swap_total)
        swap_used = swap_total;
    *swap_total_mb = (long)(swap_total / 1048576ULL);
    *swap_used_mb = (long)(swap_used / 1048576ULL);
    *swap_pct = swap_total > 0 ? 100.0 * (double)swap_used / (double)swap_total : 0.0;
    return true;
}

static bool read_load(double *l1, double *l5, double *l15) {
    /* Windows has no load average. */
    (void)l1;
    (void)l5;
    (void)l15;
    return false;
}

static long ncpus(void) {
    SYSTEM_INFO si;

    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (long)si.dwNumberOfProcessors : 1;
}

static bool read_temp_c(double *out) {
    /* Temperatures need WMI or a vendor driver; not read on Windows yet. */
    (void)out;
    return false;
}

static bool read_cpu(unsigned long long *busy, unsigned long long *total) {
    FILETIME idle;
    FILETIME kern;
    FILETIME user;
    unsigned long long i;
    unsigned long long k;
    unsigned long long u;

    if (!GetSystemTimes(&idle, &kern, &user))
        return false;
    i = win_filetime(&idle);
    k = win_filetime(&kern);
    u = win_filetime(&user);
    *total = k + u; /* kernel time already includes idle time */
    *busy = *total - i;
    return *total > 0;
}

static bool net_bytes(unsigned long long *rx, unsigned long long *tx) {
    MIB_IF_TABLE2 *table = NULL;
    unsigned long long r = 0;
    unsigned long long t = 0;
    bool any = false;

    if (GetIfTable2(&table) != NO_ERROR || table == NULL)
        return false;
    for (ULONG i = 0; i < table->NumEntries; i++) {
        const MIB_IF_ROW2 *row = &table->Table[i];

        if (row->Type == IF_TYPE_SOFTWARE_LOOPBACK || row->OperStatus != IfOperStatusUp)
            continue;
        r += row->InOctets;
        t += row->OutOctets;
        any = true;
    }
    FreeMibTable(table);
    *rx = r;
    *tx = t;
    return any;
}

static bool read_uptime(double *secs) {
    *secs = (double)GetTickCount64() / 1000.0;
    return true;
}

static int count_procs(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    int n = 0;

    if (snap == INVALID_HANDLE_VALUE)
        return -1;
    memset(&pe, 0, sizeof pe);
    pe.dwSize = sizeof pe;
    if (Process32First(snap, &pe)) {
        do {
            n++;
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return n;
}

/* Usage of every fixed drive, like df. */
static int disk_scan(disk_t *out, int max) {
    DWORD mask = GetLogicalDrives();
    int n = 0;

    for (int i = 0; i < 26 && n < max; i++) {
        char root[4];
        char fs[64] = "";
        ULARGE_INTEGER avail;
        ULARGE_INTEGER total;
        ULARGE_INTEGER freeb;
        double used;
        double av;

        if (!(mask & (1UL << i)))
            continue;
        root[0] = (char)('A' + i);
        root[1] = ':';
        root[2] = '\\';
        root[3] = '\0';
        if (GetDriveTypeA(root) != DRIVE_FIXED)
            continue;
        if (!GetVolumeInformationA(root, NULL, 0, NULL, NULL, NULL, fs, (DWORD)sizeof fs))
            continue;
        if (!GetDiskFreeSpaceExA(root, &avail, &total, &freeb) || total.QuadPart == 0)
            continue;
        used = (double)(total.QuadPart - freeb.QuadPart);
        av = (double)avail.QuadPart;
        if (used + av <= 0)
            continue;
        snprintf(out[n].mount, sizeof out[n].mount, "%s", root);
        snprintf(out[n].fstype, sizeof out[n].fstype, "%s", fs);
        out[n].pct = 100.0 * used / (used + av);
        n++;
    }
    return n;
}

#else

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

#endif

static bool cpu_busy_pct(int seconds, double *pct) {
    unsigned long long b1;
    unsigned long long t1;
    unsigned long long b2;
    unsigned long long t2;

    if (!read_cpu(&b1, &t1))
        return false;
    plat_sleep_ms(seconds * 1000);
    if (!read_cpu(&b2, &t2) || t2 <= t1)
        return false;
    *pct = 100.0 * (double)(b2 - b1) / (double)(t2 - t1);
    return true;
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

/* The first local time after `now` that is minute_of_day minutes past midnight. */
static time_t next_daily(time_t now, int minute_of_day) {
    struct tm lt;
    time_t nxt;

    if (!plat_localtime(now, &lt))
        return now + 86400;
    lt.tm_hour = 0;
    lt.tm_min = 0;
    lt.tm_sec = 0;
    lt.tm_isdst = -1;
    nxt = mktime(&lt) + (time_t)minute_of_day * 60;
    while (nxt <= now)
        nxt += 86400;
    return nxt;
}

/* True when `now` (server local time) is inside the chat's quiet hours. */
static bool in_quiet_hours(const chat_state *c, time_t now) {
    struct tm lt;
    int hour;

    if (c->quiet_start < 0 || !plat_localtime(now, &lt))
        return false;
    hour = lt.tm_hour;
    if (c->quiet_start < c->quiet_end)
        return hour >= c->quiet_start && hour < c->quiet_end;
    return hour >= c->quiet_start || hour < c->quiet_end; /* wraps past midnight */
}

/*
 * Raises and clears alerts. Quiet hours and the per-metric cooldown suppress ALERT
 * messages; a RECOVERED message is sent only after an ALERT was sent, and says how
 * long the alert lasted.
 */
static void evaluate_alerts(const metrics_t *m, time_t now) {
    for (int i = 0; i < g_nchats; i++) {
        chat_state *c = &g_chats[i];
        bool quiet = in_quiet_hours(c, now);

        for (int k = 0; k < METRIC_COUNT; k++) {
            double hyst = k == METRIC_LOAD ? 0.2 : 5.0;
            char vt[48];
            char tt[48];

            if (c->thr[k] < 0 || !m->ok[k])
                continue;
            if (!c->fired[k] && m->v[k] >= c->thr[k]) {
                bool cooling = c->cooldown_min > 0 &&
                               difftime(now, c->last_alert[k]) < c->cooldown_min * 60.0;

                c->fired[k] = true;
                c->fired_at[k] = now;
                if (quiet || cooling)
                    continue; /* the alert stays raised; no message this time */
                c->last_alert[k] = now;
                c->notified[k] = true;
                metric_text(k, m->v[k], vt, sizeof vt);
                metric_text(k, c->thr[k], tt, sizeof tt);
                reply(c->chat, "ALERT %s is %s, at or above the limit of %s",
                      metric_name(k), vt, tt);
            } else if (c->fired[k] && m->v[k] < c->thr[k] - hyst) {
                bool was_notified = c->notified[k];
                char held[48];

                c->fired[k] = false;
                c->notified[k] = false;
                if (was_notified && !quiet) {
                    metric_text(k, m->v[k], vt, sizeof vt);
                    fmt_duration(difftime(now, c->fired_at[k]), held, sizeof held);
                    reply(c->chat, "RECOVERED %s is back to %s after %s",
                          metric_name(k), vt, held);
                }
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
             reboot_required() ? "yes" : "no");
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

static const char *grade(double v, double warn, double fail) {
    if (v >= fail)
        return "FAIL";
    if (v >= warn)
        return "WARN";
    return "OK";
}

#ifdef _WIN32
static bool read_psi_some(const char *name, double *out) {
    (void)name;
    (void)out;
    return false; /* pressure stall information is Linux-only */
}
#else
/* 10-second average of 'some' pressure (percent) from /proc/pressure. */
static bool read_psi_some(const char *name, double *out) {
    char path[128];
    char line[256];
    FILE *f;
    bool found = false;

    snprintf(path, sizeof path, "/proc/pressure/%s", name);
    f = fopen(path, "r");
    if (!f)
        return false;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "some ", 5) == 0) {
            const char *p = strstr(line, "avg10=");

            if (p) {
                *out = strtod(p + 6, NULL);
                found = true;
            }
            break;
        }
    }
    fclose(f);
    return found;
}
#endif


static void diag_line(buf_t *b, int *warnings, int *failures, const char *level,
                      const char *text) {
    bappendf(b, "%-4s %s\n", level, text);
    if (strcmp(level, "WARN") == 0)
        (*warnings)++;
    else if (strcmp(level, "FAIL") == 0)
        (*failures)++;
}

/* Checks the same readings as /health, and gives each one a verdict. */
static void build_diagnose(buf_t *b) {
    double mem_pct = 0;
    double swap_pct = 0;
    double l1 = 0;
    double l5 = 0;
    double l15 = 0;
    double temp = 0;
    double psi = 0;
    long mt;
    long mu;
    long st;
    long su;
    disk_t disks[MAX_DISKS];
    int nd;
    int worst = -1;
    int warnings = 0;
    int failures = 0;
    char t[200];
    buf_t out = {0};
    char *fu[] = {"systemctl", "--failed", "--no-legend", "--plain", NULL};
    char *tu[] = {"timedatectl", "show", "-p", "NTPSynchronized", "--value", NULL};
    char *ua[] = {"apt", "list", "--upgradable", NULL};

    bappendf(b, "Diagnosis (Super_RCT %s)\n", SRCT_VERSION);

    if (read_meminfo(&mem_pct, &swap_pct, &mt, &mu, &st, &su)) {
        snprintf(t, sizeof t, "Memory %.1f%% used", mem_pct);
        diag_line(b, &warnings, &failures, grade(mem_pct, 90, 97), t);
        if (st > 0) {
            snprintf(t, sizeof t, "Swap %.1f%% used", swap_pct);
            diag_line(b, &warnings, &failures, grade(swap_pct, 50, 80), t);
        }
    }
    if (read_psi_some("memory", &psi)) {
        snprintf(t, sizeof t, "Memory pressure (10 s) %.2f%%", psi);
        diag_line(b, &warnings, &failures, grade(psi, 10, 25), t);
    }
    nd = disk_scan(disks, MAX_DISKS);
    for (int i = 0; i < nd; i++)
        if (worst < 0 || disks[i].pct > disks[worst].pct)
            worst = i;
    if (worst >= 0) {
        snprintf(t, sizeof t, "Disk %.1f%% used on %s", disks[worst].pct, disks[worst].mount);
        diag_line(b, &warnings, &failures, grade(disks[worst].pct, 90, 98), t);
    }
    if (read_load(&l1, &l5, &l15)) {
        double per_core = l1 / (double)ncpus();

        snprintf(t, sizeof t, "Load %.2f per core", per_core);
        diag_line(b, &warnings, &failures, grade(per_core, 1.0, 2.0), t);
    }
    if (read_temp_c(&temp)) {
        snprintf(t, sizeof t, "Temperature %.1f C (hottest zone)", temp);
        diag_line(b, &warnings, &failures, grade(temp, 80, 90), t);
    }
    if (capture_ok(fu, &out)) {
        int failed = count_lines(&out, "");

        snprintf(t, sizeof t, "Failed units: %d", failed);
        diag_line(b, &warnings, &failures, grade(failed, 1, 3), t);
    }
    free(out.data);
    out = (buf_t){0};
    if (capture_ok(tu, &out)) {
        bool synced = out.data && strncmp(out.data, "yes", 3) == 0;

        diag_line(b, &warnings, &failures, synced ? "OK" : "WARN",
                  synced ? "Clock is synchronised" : "Clock is not synchronised");
    }
    free(out.data);
    out = (buf_t){0};
    if (reboot_required())
        diag_line(b, &warnings, &failures, "WARN", "Reboot required");
    if (capture_ok(ua, &out)) {
        int updates = count_lines(&out, "[upgradable");

        snprintf(t, sizeof t, "Updates available: %d", updates);
        diag_line(b, &warnings, &failures, updates ? "INFO" : "OK", t);
    }
    free(out.data);
    bappendf(b, "Result: %d failure(s), %d warning(s)\n", failures, warnings);
}

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
        evaluate_alerts(&m, now);
    }

    for (int i = 0; i < g_nchats; i++) {
        chat_state *c = &g_chats[i];

        if ((c->report_every > 0 || c->daily_at >= 0) && now >= c->report_next) {
            c->report_next = c->daily_at >= 0 ? next_daily(now, c->daily_at)
                                              : now + c->report_every;
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
    plat_sleep_ms(1000);
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
#ifdef _WIN32
    reply(chat, "Failed-unit checks use systemd, which this Windows build does not have.");
    return;
#endif
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
#ifdef _WIN32
    reply(chat, "Package update checks use apt, which this Windows build does not have.");
    return;
#endif
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
          reboot_required() ? "yes" : "no");
}

#define STATE_HEADER \
    "# Super_RCT settings (saved by the bot; do not edit while it runs).\n" \
    "# chat mem disk temp load report_minutes   (-1 = alert off, 0 = no report)\n"

static bool state_value_ok(int m, double v) {
    return v == -1.0 || (v >= 0 && metric_range(m, v));
}

/* Applies saved alert limits and report intervals. A missing file is normal on
 * first run. Bad lines are skipped with a warning, so one damaged line cannot
 * stop the bot starting. */
static void load_state(void) {
    FILE *f = fopen(g_statefile, "r");
    char line[256];

    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        long long chat;
        double thr[METRIC_COUNT];
        long mins;
        int cool = 0, qs = -1, qe = -1, daily = -1;
        int used = 0;
        int n;
        const char *rest;
        chat_state *c = NULL;

        if (line[0] == '#' || line[0] == '\n')
            continue;
        /*
         * Newest lines have 10 fields. Older lines have 9 (no daily report) or 6
         * (no cooldown, quiet hours or daily report). Missing fields take defaults.
         */
        n = sscanf(line, "%lld %lf %lf %lf %lf %ld %d %d %d %d%n", &chat, &thr[0], &thr[1],
                   &thr[2], &thr[3], &mins, &cool, &qs, &qe, &daily, &used);
        if (n != 10) {
            cool = 0;
            qs = -1;
            qe = -1;
            daily = -1;
            used = 0;
            n = sscanf(line, "%lld %lf %lf %lf %lf %ld %d %d %d%n", &chat, &thr[0], &thr[1],
                       &thr[2], &thr[3], &mins, &cool, &qs, &qe, &used);
        }
        if (n != 9 && n != 10) {
            cool = 0;
            qs = -1;
            qe = -1;
            daily = -1;
            used = 0;
            n = sscanf(line, "%lld %lf %lf %lf %lf %ld%n", &chat, &thr[0], &thr[1],
                       &thr[2], &thr[3], &mins, &used);
        }
        rest = line + used;
        if ((n != 6 && n != 9 && n != 10) || rest[strspn(rest, " \t\r\n")] != '\0') {
            fprintf(stderr, "warning: skipping bad line in %s\n", g_statefile);
            continue;
        }
        for (int i = 0; i < g_nchats; i++) {
            if (g_chats[i].chat == (int64_t)chat)
                c = &g_chats[i];
        }
        if (!c)
            continue; /* chat no longer allowed in the config */

        {
            bool ok = (mins == 0 || (mins >= REPORT_MIN_MIN && mins <= REPORT_MAX_MIN));

            for (int k = 0; k < METRIC_COUNT; k++)
                ok = ok && state_value_ok(k, thr[k]);
            ok = ok && cool >= 0 && cool <= COOLDOWN_MAX_MIN;
            ok = ok && ((qs == -1 && qe == -1) ||
                        (qs >= 0 && qs <= 23 && qe >= 0 && qe <= 23 && qs != qe));
            ok = ok && (daily == -1 || (daily >= 0 && daily <= 1439));
            if (!ok) {
                fprintf(stderr, "warning: skipping out-of-range settings for chat %lld in %s\n",
                        chat, g_statefile);
                continue;
            }
        }
        for (int k = 0; k < METRIC_COUNT; k++) {
            c->thr[k] = thr[k];
            c->fired[k] = false;
            c->notified[k] = false;
        }
        c->report_every = (time_t)mins * 60;
        c->report_next = time(NULL) + c->report_every;
        c->cooldown_min = cool;
        c->quiet_start = qs;
        c->quiet_end = qe;
        c->daily_at = daily;
        /* Never "now": a restart must not send a report at once. */
        if (daily >= 0)
            c->report_next = next_daily(time(NULL), daily);
    }
    fclose(f);
}

/* Writes non-default settings atomically with mode 0600. */
static bool save_state(void) {
    char tmp[MAX_URL + 8];
    int fd;
    FILE *f;
    bool ok;

    snprintf(tmp, sizeof tmp, "%s.tmp", g_statefile);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return false;
    f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp);
        return false;
    }
    fputs(STATE_HEADER, f);
    for (int i = 0; i < g_nchats; i++) {
        const chat_state *c = &g_chats[i];
        bool any = c->report_every > 0 || c->cooldown_min > 0 || c->quiet_start >= 0 ||
                   c->daily_at >= 0;

        for (int k = 0; k < METRIC_COUNT; k++) {
            if (c->thr[k] >= 0)
                any = true;
        }
        if (!any)
            continue;
        fprintf(f, "%lld", (long long)c->chat);
        for (int k = 0; k < METRIC_COUNT; k++)
            fprintf(f, " %.17g", c->thr[k]);
        fprintf(f, " %ld %d %d %d %d\n", (long)(c->report_every / 60), c->cooldown_min,
                c->quiet_start, c->quiet_end, c->daily_at);
    }
    ok = fflush(f) == 0 && plat_fsync(f);
    if (fclose(f) != 0)
        ok = false;
    if (!ok || !plat_replace_file(tmp, g_statefile)) {
        unlink(tmp);
        return false;
    }
    return true;
}

static void persist_settings(int64_t chat) {
    if (!save_state())
        reply(chat, "Warning: settings could not be saved and will reset on restart.");
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
        if (c->cooldown_min > 0)
            bappendf(&b, "Cooldown between alerts: %d min\n", c->cooldown_min);
        else
            bappendf(&b, "Cooldown between alerts: off\n");
        if (c->quiet_start >= 0)
            bappendf(&b, "Quiet hours (server time): %02d:00 to %02d:00\n", c->quiet_start,
                     c->quiet_end);
        else
            bappendf(&b, "Quiet hours: off\n");
        reply_buf(chat, &b);
        buf_free(&b);
        return;
    }

    if (strcmp(sub, "cooldown") == 0) {
        char *end = NULL;
        long v = strtol(name, &end, 10);

        if (name[0] == '\0' || *end != '\0' || v < 0 || v > COOLDOWN_MAX_MIN) {
            reply(chat, "Usage: /alert cooldown <minutes 0-%d> (0 = off)", COOLDOWN_MAX_MIN);
            return;
        }
        c->cooldown_min = (int)v;
        persist_settings(chat);
        if (v > 0)
            reply(chat, "Alert cooldown set to %ld minutes.", v);
        else
            reply(chat, "Alert cooldown turned off.");
        return;
    }

    if (strcmp(sub, "quiet") == 0) {
        char *e1 = NULL, *e2 = NULL;
        long a, b2;

        if (strcmp(name, "off") == 0) {
            c->quiet_start = c->quiet_end = -1;
            persist_settings(chat);
            reply(chat, "Quiet hours turned off.");
            return;
        }
        a = strtol(name, &e1, 10);
        b2 = strtol(val, &e2, 10);
        if (name[0] == '\0' || val[0] == '\0' || *e1 != '\0' || *e2 != '\0' ||
            a < 0 || a > 23 || b2 < 0 || b2 > 23 || a == b2) {
            reply(chat, "Usage: /alert quiet <start hour 0-23> <end hour 0-23> "
                        "(server time, end not equal to start) | /alert quiet off");
            return;
        }
        c->quiet_start = (int)a;
        c->quiet_end = (int)b2;
        persist_settings(chat);
        reply(chat, "Quiet hours set: no alerts from %02ld:00 to %02ld:00 (server time).",
              a, b2);
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
        c->notified[m] = false;
        persist_settings(chat);
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
        c->notified[m] = false;
        persist_settings(chat);
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

    reply(chat, "Usage: /alert list | set <metric> <value> | off <metric> | check | "
                "cooldown <minutes> | quiet <start> <end> | quiet off");
}

static void cmd_report(int64_t chat, chat_state *c, const char *arg, time_t now) {
    char sub[16] = "";
    char val[16] = "";
    char clock[8];

    (void)sscanf(arg, "%15s %15s", sub, val);

    if (sub[0] == '\0' || strcmp(sub, "status") == 0) {
        if (c->daily_at >= 0) {
            fmt_clock(c->report_next, clock, sizeof clock);
            reply(chat, "Daily health report at %02d:%02d (server time); next at %s.",
                  c->daily_at / 60, c->daily_at % 60, clock);
        } else if (c->report_every <= 0) {
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
        c->daily_at = -1;
        persist_settings(chat);
        reply(chat, "Health reports on: every %ld minutes.", mins);
        return;
    }
    if (strcmp(sub, "daily") == 0) {
        int hh = -1, mm = -1;
        char extra = '\0';

        if (sscanf(val, "%d:%d%c", &hh, &mm, &extra) != 2 || hh < 0 || hh > 23 ||
            mm < 0 || mm > 59) {
            reply(chat, "Usage: /report daily <HH:MM>  (server time, 00:00-23:59)");
            return;
        }
        c->daily_at = hh * 60 + mm;
        c->report_every = 0;
        c->report_next = next_daily(now, c->daily_at);
        persist_settings(chat);
        reply(chat, "Daily health report at %02d:%02d (server time).", hh, mm);
        return;
    }
    if (strcmp(sub, "off") == 0) {
        c->report_every = 0;
        c->daily_at = -1;
        persist_settings(chat);
        reply(chat, "Scheduled reports turned off.");
        return;
    }
    if (strcmp(sub, "now") == 0) {
        send_health(chat);
        return;
    }
    reply(chat, "Usage: /report status | on <minutes> | daily <HH:MM> | off | now");
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
    bappendf(&b, "Host statistics readable: %s\n", host_stats_readable() ? "PASS" : "FAIL");
    bappendf(&b, "Disk statistics: %s\n", disk_stats_ok() ? "PASS" : "FAIL");
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

/* /run <command>: runs the command through /bin/sh on this host (SUPER_RCT_SHELL). */
static void cmd_run(int64_t chat, const char *arg) {
    const char *p = arg;
    size_t n, cut;
    char *cmd;
    buf_t body = {0}, msg = {0};
    char note[96] = "";
    int status;

    if (!g_shell_enabled) {
        reply(chat, "/run is disabled on this host (SUPER_RCT_SHELL=0).");
        return;
    }
    while (*p == ' ' || *p == '\t')
        p++;
    n = strlen(p);
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t'))
        n--;
    if (n == 0) {
        reply(chat, "Usage: /run <command>  (runs through /bin/sh on this host)");
        return;
    }
    if (n > SHELL_MAX_CMD) {
        reply(chat, "Command is too long (max %d bytes).", SHELL_MAX_CMD);
        return;
    }
    cmd = malloc(n + 1);
    if (!cmd) {
        reply(chat, "Out of memory.");
        return;
    }
    memcpy(cmd, p, n);
    cmd[n] = '\0';

    status = run_shell_cmd(cmd, SHELL_TIMEOUT_S, &body);
    /* Audit line: who ran what and the result. Output is not logged. */
    logf_("run chat=%" PRId64 " status=%d cmd=\"%s\"", chat, status, cmd);
    free(cmd);

    if (status < 0) {
        reply(chat, "Could not run /run");
        buf_free(&body);
        return;
    }
    if (status == EXIT_TIMEOUT)
        snprintf(note, sizeof note, "\n(timed out after %d s)", SHELL_TIMEOUT_S);
    else if (status == EXIT_NOT_FOUND)
        snprintf(note, sizeof note, "\n(could not run /bin/sh)");
    else if (status > 0)
        snprintf(note, sizeof note, "\n(exit %d)", status);

    n = body.len;
    while (n > 0 && (body.data[n - 1] == '\n' || body.data[n - 1] == '\r' ||
                     body.data[n - 1] == ' ' || body.data[n - 1] == '\t'))
        n--;
    body.len = n;

    buf_add(&msg, "/run\n", 5);
    if (body.len == 0) {
        buf_add(&msg, "(no output)", 11);
    } else {
        cut = utf8_cut(body.data, body.len, OUT_MAX);
        buf_add(&msg, body.data, cut);
        if (cut < body.len)
            buf_add(&msg, "\n... (truncated)", 16);
    }
    buf_add(&msg, note, strlen(note));
    send_text(chat, msg.data);
    buf_free(&body);
    buf_free(&msg);
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
    else if (strcmp(cmd, "/diagnose") == 0) {
        buf_t b = {0};

        build_diagnose(&b);
        reply_buf(chat, &b);
        buf_free(&b);
    }
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
    else if (strcmp(cmd, "/run") == 0)
        cmd_run(chat, arg);
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
