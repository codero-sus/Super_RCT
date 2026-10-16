/*
 * Super_RCT - a small Telegram remote-control bot (C variant).
 *
 * Features
 *   - Long-polls the Telegram Bot API with getUpdates.
 *   - Only chats listed in SUPER_RCT_CHAT_IDS may talk to the bot.
 *   - Password login per chat: /auth <password>, idle-timeout sessions,
 *     /logout, and a lockout after repeated wrong passwords.
 *   - Built-in commands: /help, /status, /ip list|enable|disable.
 *   - Keeps an IP registry (enabled/disabled) in a plain text file.
 *
 * Design notes
 *   - POSIX only (Linux, macOS, BSD). No runtime libraries besides libc.
 *   - HTTPS is delegated to the system `curl` executable. Requests are
 *     passed to curl through its config-file interface on stdin, so the
 *     bot token and password never appear in the process list.
 *   - JSON is parsed with a small depth-aware reader that only understands
 *     the subset of fields the bot needs.
 *   - /run runs a shell command for an authenticated chat. SUPER_RCT_SHELL=0 turns it off.
 *
 * Configuration (environment variables)
 *   SUPER_RCT_TOKEN            Telegram bot token (required)
 *   SUPER_RCT_CHAT_IDS         Comma-separated allowed chat IDs (required)
 *   SUPER_RCT_PASSWORD         Login password, 8-128 bytes (required)
 *   SUPER_RCT_SESSION_SECONDS  Idle timeout for a login (default 3600)
 *   SUPER_RCT_IP_FILE          IP registry file (default: super_rct_ips.txt)
 *   SUPER_RCT_STATE_FILE      saved alert and report settings (default: super_rct_state.txt)
 *   SUPER_RCT_API              API base URL (default: https://api.telegram.org)
 *   SUPER_RCT_SHELL            1 (default) allows /run; 0 turns it off
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "commands_table.h"

#define SRCT_VERSION "0.5.0"

#define DEFAULT_API "https://api.telegram.org"
#define DEFAULT_IP_FILE "super_rct_ips.txt"
#define DEFAULT_STATE_FILE "super_rct_state.txt"
#define DEFAULT_SESSION_S 3600
#define MIN_SESSION_S 5
#define MAX_SESSION_S 86400
#define MIN_PASSWORD 8
#define MAX_PASSWORD 128
#define MAX_TOKEN 256
#define MAX_URL 512
#define MAX_CHATS 32
#define MAX_IPS 256
#define MAX_TEXT 1024
#define IP_LEN 64
#define POLL_TIMEOUT_S 25
#define RETRY_DELAY_S 5
#define MAX_FAILS 3
#define LOCKOUT_S 300

/* ------------------------------------------------------------------------- */
/* Global state                                                              */
/* ------------------------------------------------------------------------- */

typedef struct {
    char ip[IP_LEN];
    bool enabled;
} ip_entry;

typedef struct {
    int64_t chat;
    time_t session_until; /* logged in while now < session_until */
    int fails;            /* consecutive wrong passwords */
    time_t locked_until;  /* refuses /auth while now < locked_until */
    double thr[4];        /* alert limits: mem, disk, temp, load (-1 = off) */
    bool fired[4];        /* alert currently raised */
    time_t report_every;  /* scheduled health report interval, 0 = off */
    time_t report_next;
    int cooldown_min;     /* minutes between ALERTs for one metric, 0 = off */
    int quiet_start;      /* quiet hours in server local time, -1 = off */
    int quiet_end;
    time_t last_alert[4]; /* when an ALERT was last sent, per metric */
    time_t fired_at[4];   /* when the current alert was raised */
    bool notified[4];     /* an ALERT was sent and not yet recovered */
    int daily_at;         /* daily report at this minute after local midnight, -1 = off */
    bool discord;         /* true for a Discord channel, false for Telegram */
    uint64_t dc_cursor;   /* newest Discord message ID handled */
    bool dc_primed;       /* cursor set from the channel's current newest message */
} chat_state;

static volatile sig_atomic_t g_stop = 0;
static int g_shell_enabled = 1; /* SUPER_RCT_SHELL: 1 = /run allowed */
static time_t g_started;
static char g_token[MAX_TOKEN];
static char g_password[MAX_PASSWORD + 1];
static char g_api[MAX_URL] = DEFAULT_API;
static char g_ipfile[MAX_URL] = DEFAULT_IP_FILE;
static char g_statefile[MAX_URL] = DEFAULT_STATE_FILE;
static long g_session_s = DEFAULT_SESSION_S;
static chat_state g_chats[MAX_CHATS];
static int g_nchats;
static int64_t g_offset;
static ip_entry g_ips[MAX_IPS];
static int g_nips;
#define MAX_WATCH 16
static char g_watch[MAX_WATCH][MAX_URL];
static int g_nwatch;
static long g_watch_max_h = 24;
static unsigned long g_cmds_handled;
static unsigned long g_auth_failures;

/* ------------------------------------------------------------------------- */
/* Logging                                                                   */
/* ------------------------------------------------------------------------- */

static void logf_(const char *fmt, ...) {
    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;
    va_list ap;

    localtime_r(&now, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(stderr, "[%s] ", ts);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ------------------------------------------------------------------------- */
/* Growable byte buffer                                                      */
/* ------------------------------------------------------------------------- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int err; /* sticky: set on allocation failure */
} buf_t;

static void buf_add(buf_t *b, const char *s, size_t n) {
    if (b->err)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        char *p;

        while (nc < b->len + n + 1)
            nc *= 2;
        p = realloc(b->data, nc);
        if (!p) {
            b->err = 1;
            return;
        }
        b->data = p;
        b->cap = nc;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_free(buf_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
    b->err = 0;
}

/*
 * Append one curl config directive: key = "value"
 * Escapes the characters that curl's config parser treats specially.
 */
static void cfg_put(buf_t *b, const char *key, const char *value) {
    buf_add(b, key, strlen(key));
    buf_add(b, " = \"", 4);
    for (const char *p = value; *p; p++) {
        switch (*p) {
        case '"':  buf_add(b, "\\\"", 2); break;
        case '\\': buf_add(b, "\\\\", 2); break;
        case '\n': buf_add(b, "\\n", 2); break;
        case '\r': buf_add(b, "\\r", 2); break;
        case '\t': buf_add(b, "\\t", 2); break;
        default:
            if ((unsigned char)*p >= 0x20)
                buf_add(b, p, 1);
            break;
        }
    }
    buf_add(b, "\"\n", 2);
}

/* ------------------------------------------------------------------------- */
/* HTTP via curl                                                             */
/* ------------------------------------------------------------------------- */

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

/*
 * Runs `curl -s -K -` with `cfg` on stdin and captures stdout into `out`.
 * Returns 0 on success, -1 on any failure.
 */
static int run_curl(const buf_t *cfg, buf_t *out) {
    int in_p[2];
    int out_p[2];
    pid_t pid;
    size_t off = 0;
    bool ok = true;
    int status = 0;
    char chunk[4096];

    if (pipe(in_p) < 0)
        return -1;
    if (pipe(out_p) < 0) {
        close(in_p[0]);
        close(in_p[1]);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(in_p[0]);
        close(in_p[1]);
        close(out_p[0]);
        close(out_p[1]);
        return -1;
    }

    if (pid == 0) {
        dup2(in_p[0], STDIN_FILENO);
        dup2(out_p[1], STDOUT_FILENO);
        close(in_p[0]);
        close(in_p[1]);
        close(out_p[0]);
        close(out_p[1]);
        execlp("curl", "curl", "-s", "-K", "-", (char *)NULL);
        _exit(127);
    }

    close(in_p[0]);
    close(out_p[1]);

    /* Config is small (well under the pipe buffer), so a blocking write is safe. */
    while (off < cfg->len) {
        ssize_t w = write(in_p[1], cfg->data + off, cfg->len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        off += (size_t)w;
    }
    close(in_p[1]);

    for (;;) {
        ssize_t r = read(out_p[0], chunk, sizeof chunk);
        if (r < 0) {
            if (errno == EINTR) {
                if (g_stop)
                    break;
                continue;
            }
            ok = false;
            break;
        }
        if (r == 0)
            break;
        buf_add(out, chunk, (size_t)r);
    }
    close(out_p[0]);

    if (g_stop)
        kill(pid, SIGTERM);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;

    if (out->err)
        ok = false;
    return (ok && WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* Builds the endpoint URL for a Bot API method. */
static void api_url(char *dst, size_t n, const char *method) {
    snprintf(dst, n, "%s/bot%s/%s", g_api, g_token, method);
}

static int tg_get_updates(int timeout_s, buf_t *out) {
    buf_t cfg = {0};
    char url[MAX_URL + MAX_TOKEN + 64];
    char kv[64];
    int rc;

    api_url(url, sizeof url, "getUpdates");
    cfg_put(&cfg, "url", url);
    /* One data-urlencode directive per field, so each is sent as its own parameter. */
    snprintf(kv, sizeof kv, "timeout=%d", timeout_s);
    cfg_put(&cfg, "data-urlencode", kv);
    snprintf(kv, sizeof kv, "offset=%" PRId64, g_offset);
    cfg_put(&cfg, "data-urlencode", kv);
    cfg_put(&cfg, "max-time", "40");

    rc = run_curl(&cfg, out);
    buf_free(&cfg);
    return rc;
}

static int tg_send_text(int64_t chat, const char *text) {
    buf_t cfg = {0};
    buf_t kv = {0};
    buf_t out = {0};
    char url[MAX_URL + MAX_TOKEN + 64];
    char num[32];
    int rc;

    api_url(url, sizeof url, "sendMessage");
    cfg_put(&cfg, "url", url);
    snprintf(num, sizeof num, "chat_id=%" PRId64, chat);
    cfg_put(&cfg, "data-urlencode", num);
    buf_add(&kv, "text=", 5);
    buf_add(&kv, text, strlen(text));
    cfg_put(&cfg, "data-urlencode", kv.data);
    cfg_put(&cfg, "max-time", "20");

    rc = run_curl(&cfg, &out);
    if (rc != 0)
        logf_("sendMessage to chat %" PRId64 " failed", chat);
    buf_free(&cfg);
    buf_free(&kv);
    buf_free(&out);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Minimal JSON reader                                                       */
/* ------------------------------------------------------------------------- */

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

/* Returns a pointer just past the JSON value starting at p. */
static const char *json_skip(const char *p) {
    p = skip_ws(p);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1])
                p++;
            p++;
        }
        return *p ? p + 1 : p;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = json_skip(p);
                continue;
            }
            if (*p == '{' || *p == '[')
                depth++;
            else if (*p == '}' || *p == ']') {
                depth--;
                if (depth == 0)
                    return p + 1;
            }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
           *p != '\t' && *p != '\n' && *p != '\r')
        p++;
    return p;
}

/*
 * Looks up a top-level key in the JSON object starting at `obj`.
 * Returns a pointer to the value, or NULL if absent.
 */
static const char *json_get(const char *obj, const char *key) {
    size_t kl = strlen(key);
    const char *p;

    obj = skip_ws(obj);
    if (*obj != '{')
        return NULL;
    p = obj + 1;
    for (;;) {
        const char *kstart;
        bool match;

        p = skip_ws(p);
        if (*p != '"')
            return NULL;
        kstart = p + 1;
        match = strncmp(kstart, key, kl) == 0 && kstart[kl] == '"';
        p = skip_ws(json_skip(p));
        if (*p != ':')
            return NULL;
        p = skip_ws(p + 1);
        if (match)
            return p;
        p = skip_ws(json_skip(p));
        if (*p == ',')
            p++;
        else
            return NULL;
    }
}

/* Decodes the JSON string at p into dst (NUL-terminated, truncated to n). */
static bool json_str(const char *p, char *dst, size_t n) {
    size_t i = 0;

    p = skip_ws(p);
    if (*p != '"' || n == 0)
        return false;
    p++;
    while (*p && *p != '"') {
        char c = *p++;

        if (c == '\\' && *p) {
            char e = *p++;

            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'u':
                for (int k = 0; k < 4 && *p; k++)
                    p++;
                c = '?'; /* non-ASCII: not needed for commands */
                break;
            default: c = e; break; /* \" \\ \/ */
            }
        }
        if (i + 1 < n)
            dst[i++] = c;
    }
    dst[i] = '\0';
    return *p == '"';
}

static bool json_int(const char *p, int64_t *out) {
    char *end;
    long long v;

    if (!p)
        return false;
    p = skip_ws(p);
    v = strtoll(p, &end, 10);
    if (end == p)
        return false;
    *out = (int64_t)v;
    return true;
}

/* ------------------------------------------------------------------------- */
/* IP registry                                                               */
/* ------------------------------------------------------------------------- */

static ip_entry *ip_find(const char *ip) {
    for (int i = 0; i < g_nips; i++)
        if (strcmp(g_ips[i].ip, ip) == 0)
            return &g_ips[i];
    return NULL;
}

#include "discord_transport.h"

static void ip_load(void) {
    FILE *f = fopen(g_ipfile, "r");
    char line[256];

    g_nips = 0;
    if (!f)
        return; /* no registry yet */
    while (fgets(line, sizeof line, f) && g_nips < MAX_IPS) {
        char state[16];
        char ip[IP_LEN];

        if (sscanf(line, "%15s %63s", state, ip) != 2)
            continue;
        strcpy(g_ips[g_nips].ip, ip);
        g_ips[g_nips].enabled = strcmp(state, "enabled") == 0;
        g_nips++;
    }
    fclose(f);
}

static bool ip_save(void) {
    char tmp[MAX_URL + 8];
    FILE *f;

    snprintf(tmp, sizeof tmp, "%s.tmp", g_ipfile);
    f = fopen(tmp, "w");
    if (!f)
        return false;
    for (int i = 0; i < g_nips; i++)
        fprintf(f, "%s %s\n", g_ips[i].enabled ? "enabled" : "disabled",
                g_ips[i].ip);
    if (fclose(f) != 0)
        return false;
    return rename(tmp, g_ipfile) == 0;
}

/* Validates and canonicalises an IPv4 or IPv6 address. */
static bool ip_normalise(const char *in, char *out, size_t n) {
    unsigned char buf[sizeof(struct in6_addr)];

    if (strchr(in, '%'))
        return false; /* no scope IDs */
    if (inet_pton(AF_INET, in, buf) == 1)
        return inet_ntop(AF_INET, buf, out, (socklen_t)n) != NULL;
    if (inet_pton(AF_INET6, in, buf) == 1)
        return inet_ntop(AF_INET6, buf, out, (socklen_t)n) != NULL;
    return false;
}

/* ------------------------------------------------------------------------- */
/* Authentication                                                            */
/* ------------------------------------------------------------------------- */

/* Compares two strings without leaking where they first differ. */
static bool ct_equal(const char *a, const char *b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    size_t n = la > lb ? la : lb;
    unsigned char diff = (unsigned char)(la != lb);

    for (size_t i = 0; i < n; i++) {
        unsigned char ca = i < la ? (unsigned char)a[i] : 0;
        unsigned char cb = i < lb ? (unsigned char)b[i] : 0;
        diff |= (unsigned char)(ca ^ cb);
    }
    return diff == 0;
}

static bool session_active(const chat_state *c, time_t now) {
    return c->session_until > now;
}

/* ------------------------------------------------------------------------- */
/* Command handlers                                                          */
/* ------------------------------------------------------------------------- */

static int chat_index(int64_t chat) {
    for (int i = 0; i < g_nchats; i++)
        if (g_chats[i].chat == chat)
            return i;
    return -1;
}

/* Sends text to whichever service the chat belongs to. */
static void send_text(int64_t chat, const char *text) {
    int i = chat_index(chat);

    if (i >= 0 && g_chats[i].discord)
        discord_send(chat, text);
    else
        tg_send_text(chat, text);
}

static void reply(int64_t chat, const char *fmt, ...) {
    char text[MAX_TEXT];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    send_text(chat, text);
}

static void cmd_help(int64_t chat) {
    reply(chat,
          "Super_RCT %s commands:\n"
          "/auth <password> - log in (required before other commands)\n"
          "/logout - end your session\n"
          "/status - host status\n"
          "/ip list - show IP registry\n"
          "/ip enable <ip> - mark an IP as enabled\n"
          "/ip disable <ip> - mark an IP as disabled\n"
          "/commands - list all commands\n"
          "/help - this message\n"
          "Note: the IP registry is informational. No firewall rules are changed.",
          SRCT_VERSION);
}

static void cmd_auth(chat_state *c, const char *arg, time_t now) {
    if (c->locked_until > now) {
        reply(c->chat, "Too many failed attempts. Try again in %ld s.",
              (long)(c->locked_until - now));
        return;
    }
    if (arg[0] == '\0') {
        reply(c->chat, "Usage: /auth <password>");
        return;
    }
    if (ct_equal(arg, g_password)) {
        c->fails = 0;
        c->session_until = now + g_session_s;
        logf_("chat %" PRId64 " authenticated", c->chat);
        reply(c->chat, "Authenticated. Session expires after %ld s of inactivity.",
              g_session_s);
        return;
    }

    c->fails++;
    g_auth_failures++;
    logf_("failed login for chat %" PRId64 " (%d/%d)", c->chat, c->fails, MAX_FAILS);
    if (c->fails >= MAX_FAILS) {
        c->fails = 0;
        c->session_until = 0;
        c->locked_until = now + LOCKOUT_S;
        reply(c->chat, "Wrong password. Too many failed attempts; locked for %d s.",
              LOCKOUT_S);
    } else {
        reply(c->chat, "Wrong password (%d/%d failed attempts).", c->fails, MAX_FAILS);
    }
}

static void cmd_logout(chat_state *c) {
    c->session_until = 0;
    reply(c->chat, "Logged out.");
}

static void cmd_status(int64_t chat) {
    char host[256] = "unknown";
    long up = (long)difftime(time(NULL), g_started);

    gethostname(host, sizeof host - 1);
    reply(chat, "Super_RCT %s\nHost: %s\nPID: %ld\nUptime: %ldh %ldm %lds\nIP entries: %d",
          SRCT_VERSION, host, (long)getpid(), up / 3600, (up % 3600) / 60, up % 60,
          g_nips);
}

static void cmd_ip(int64_t chat, const char *args) {
    char sub[64] = "";
    char ip_in[128] = "";

    (void)sscanf(args, "%63s %127s", sub, ip_in);

    if (sub[0] == '\0' || strcmp(sub, "list") == 0) {
        char list[MAX_TEXT] = "";
        size_t used = 0;

        if (g_nips == 0) {
            reply(chat, "IP registry is empty.");
            return;
        }
        for (int i = 0; i < g_nips; i++) {
            int w = snprintf(list + used, sizeof list - used, "%s %s\n",
                             g_ips[i].ip, g_ips[i].enabled ? "enabled" : "disabled");
            if (w < 0 || (size_t)w >= sizeof list - used)
                break;
            used += (size_t)w;
        }
        reply(chat, "%s", list);
        return;
    }

    if (strcmp(sub, "enable") == 0 || strcmp(sub, "disable") == 0) {
        bool enable = strcmp(sub, "enable") == 0;
        char ip[IP_LEN];
        ip_entry *e;

        if (ip_in[0] == '\0') {
            reply(chat, "Usage: /ip %s <ip>", sub);
            return;
        }
        if (!ip_normalise(ip_in, ip, sizeof ip)) {
            reply(chat, "Invalid IP address: %s", ip_in);
            return;
        }
        e = ip_find(ip);
        if (!e) {
            if (g_nips >= MAX_IPS) {
                reply(chat, "IP registry is full (%d entries).", MAX_IPS);
                return;
            }
            e = &g_ips[g_nips++];
            strcpy(e->ip, ip);
        }
        e->enabled = enable;
        if (!ip_save()) {
            logf_("failed to write %s", g_ipfile);
            reply(chat, "Could not save the IP registry. See the bot log.");
            return;
        }
        reply(chat, "%s is now %s.", ip, enable ? "enabled" : "disabled");
        return;
    }

    reply(chat, "Usage: /ip list | /ip enable <ip> | /ip disable <ip>");
}

/*
 * Splits "/cmd rest of line" into the command word and the trimmed remainder.
 * The remainder keeps internal spaces, so passwords may contain spaces.
 */
static void split_command(const char *text, char *cmd, size_t cn, char *arg, size_t an) {
    const char *p = text;
    size_t i = 0;
    size_t n;

    while (*p == ' ' || *p == '\t')
        p++;
    while (*p && *p != ' ' && *p != '\t' && i + 1 < cn)
        cmd[i++] = *p++;
    cmd[i] = '\0';
    while (*p == ' ' || *p == '\t')
        p++;

    snprintf(arg, an, "%s", p);
    n = strlen(arg);
    while (n > 0 && (arg[n - 1] == ' ' || arg[n - 1] == '\t' ||
                     arg[n - 1] == '\r' || arg[n - 1] == '\n'))
        arg[--n] = '\0';
}

/* ------------------------------------------------------------------------- */
/* Table commands: fixed files and fixed programs. Never a shell.            */
/* ------------------------------------------------------------------------- */

#define CMD_TIMEOUT_S 5
#define SHELL_TIMEOUT_S 60 /* /run: one shell command */
#define SHELL_MAX_CMD 1000 /* bytes in the command text */
#define READ_MAX 65536
#define OUT_MAX 3500 /* body cap, well under Telegram's 4096 characters */
#define CHUNK_MAX 3500
#define EXIT_NOT_FOUND 127
#define EXIT_TIMEOUT 124

/* Splits a fixed target such as "ps -eo pid,comm" into argv. No quoting. */
static bool split_argv(const char *target, char *store, size_t n, char **argv, int max) {
    size_t len = strlen(target);
    int k = 0;
    char *p;

    if (len + 1 > n)
        return false;
    memcpy(store, target, len + 1);
    p = store;
    for (;;) {
        while (*p == ' ')
            *p++ = '\0';
        if (*p == '\0')
            break;
        if (k >= max - 1)
            return false;
        argv[k++] = p;
        while (*p && *p != ' ')
            p++;
    }
    argv[k] = NULL;
    return k > 0;
}

/*
 * Runs a fixed program (no shell), capturing stdout and stderr up to READ_MAX.
 * Returns the exit status, EXIT_TIMEOUT if killed by the alarm, or -1 on error.
 */
static int run_argv_vec(char *const *argv, buf_t *out) {
    int pfd[2];
    pid_t pid;
    int status = 0;
    char chunk[4096];

    if (pipe(pfd) < 0)
        return -1;

    pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        static const char missing[] = "command not available on this host\n";
        int nul = open("/dev/null", O_RDONLY);

        if (nul >= 0) {
            dup2(nul, STDIN_FILENO);
            close(nul);
        }
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        alarm(CMD_TIMEOUT_S);
        execvp(argv[0], argv);
        {
            ssize_t w = write(STDOUT_FILENO, missing, sizeof missing - 1);
            (void)w; /* best effort: the child exits either way */
        }
        _exit(EXIT_NOT_FOUND);
    }

    close(pfd[1]);
    for (;;) {
        ssize_t r = read(pfd[0], chunk, sizeof chunk);

        if (r < 0) {
            if (errno == EINTR) {
                if (g_stop)
                    break;
                continue;
            }
            break;
        }
        if (r == 0)
            break;
        if (out->len < READ_MAX) {
            size_t take = (size_t)r;

            if (take > READ_MAX - out->len)
                take = READ_MAX - out->len;
            buf_add(out, chunk, take);
        }
        /* Beyond READ_MAX we keep draining so the child never blocks. */
    }
    close(pfd[0]);

    if (g_stop)
        kill(pid, SIGTERM);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (WIFSIGNALED(status))
        return EXIT_TIMEOUT;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

static int run_argv(const char *target, buf_t *out) {
    char store[256];
    char *argv[16];

    if (!split_argv(target, store, sizeof store, argv, 16))
        return -1;
    return run_argv_vec(argv, out);
}

static double mono_seconds(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/*
 * Runs `cmd` with /bin/sh -c in its own process group (for /run). Output is kept up
 * to READ_MAX bytes and the rest is drained. If the command, or a background child
 * that still holds the output pipe, outlives `timeout_s`, the whole group is killed
 * and EXIT_TIMEOUT is returned. Returns -1 on a local error.
 */
static int run_shell_cmd(const char *cmd, int timeout_s, buf_t *out) {
    int pfd[2];
    pid_t pid;
    int status = 0;
    int exited = 0, eof = 0, killed = 0;
    double deadline;
    char chunk[4096];

    if (pipe(pfd) < 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDONLY);

        if (nul >= 0) {
            dup2(nul, STDIN_FILENO);
            close(nul);
        }
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(EXIT_NOT_FOUND);
    }
    setpgid(pid, pid); /* also in the parent, so the group exists before any kill */
    close(pfd[1]);

    deadline = mono_seconds() + timeout_s;
    while (!(eof && exited)) {
        struct pollfd pf;
        double left = deadline - mono_seconds();
        int wait_ms;

        if (!exited && waitpid(pid, &status, WNOHANG) == pid)
            exited = 1;
        if (left <= 0 || g_stop) {
            kill(-pid, SIGKILL);
            killed = 1;
            break;
        }
        if (eof) { /* output closed, waiting for the child to exit */
            struct timespec nap = {0, 50000000};

            nanosleep(&nap, NULL);
            continue;
        }
        pf.fd = pfd[0];
        pf.events = POLLIN;
        pf.revents = 0;
        wait_ms = (int)(left * 1000.0);
        if (wait_ms > 200)
            wait_ms = 200;
        if (wait_ms < 1)
            wait_ms = 1;
        if (poll(&pf, 1, wait_ms) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pf.revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t r = read(pfd[0], chunk, sizeof chunk);

            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0) {
                eof = 1;
            } else if (out->len < READ_MAX) {
                size_t take = (size_t)r;

                if (take > READ_MAX - out->len)
                    take = READ_MAX - out->len;
                buf_add(out, chunk, take);
            }
        }
    }
    close(pfd[0]);
    if (!exited) {
        if (!killed) {
            kill(-pid, SIGKILL); /* poll failed; do not leave the child behind */
            killed = 1;
        }
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
    }
    if (killed)
        return EXIT_TIMEOUT;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

/* Reads at most READ_MAX bytes of a fixed file. Returns false if unreadable. */
static bool read_file_capped(const char *path, buf_t *out) {
    FILE *f = fopen(path, "r");
    char chunk[4096];

    if (!f)
        return false;
    for (;;) {
        size_t n;
        size_t take;

        if (out->len >= READ_MAX)
            break;
        n = fread(chunk, 1, sizeof chunk, f);
        if (n == 0)
            break;
        take = n;
        if (take > READ_MAX - out->len)
            take = READ_MAX - out->len;
        buf_add(out, chunk, take);
    }
    fclose(f);
    return true;
}

/* Byte index to cut a UTF-8 string at, never inside a multi-byte character. */
static size_t utf8_cut(const char *s, size_t len, size_t max) {
    if (len <= max)
        return len;
    while (max > 0 && ((unsigned char)s[max] & 0xC0) == 0x80)
        max--;
    return max;
}

/* Sends text in chunks of whole lines, each under Telegram's limit. */
static void send_long(int64_t chat, const char *text) {
    buf_t chunk = {0};
    const char *p = text;

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) + 1 : strlen(p);

        if (chunk.len && chunk.len + len > CHUNK_MAX) {
            send_text(chat, chunk.data);
            buf_free(&chunk);
        }
        buf_add(&chunk, p, len);
        p += len;
    }
    if (chunk.len)
        send_text(chat, chunk.data);
    buf_free(&chunk);
}

static const rct_cmd_def *find_table_cmd(const char *name) {
    for (int i = 0; i < RCT_NCOMMANDS; i++)
        if (RCT_COMMANDS[i].kind != 'b' && strcmp(RCT_COMMANDS[i].name, name) == 0)
            return &RCT_COMMANDS[i];
    return NULL;
}

static void run_table_command(int64_t chat, const rct_cmd_def *def) {
    buf_t body = {0};
    buf_t msg = {0};
    char note[32] = "";
    size_t n;
    size_t cut;
    char head[80];

    if (def->kind == 'f') {
        if (!read_file_capped(def->target, &body)) {
            reply(chat, "/%s\nNot available on this host: %s", def->name, def->target);
            buf_free(&body);
            return;
        }
    } else {
        int status = run_argv(def->target, &body);

        if (status == EXIT_TIMEOUT)
            snprintf(note, sizeof note, "(timed out)");
        else if (status > 0 && status != EXIT_NOT_FOUND)
            snprintf(note, sizeof note, "(exit %d)", status);
        else if (status < 0) {
            reply(chat, "Could not run /%s", def->name);
            buf_free(&body);
            return;
        }
    }

    /* Trim trailing whitespace, then add any status note. */
    n = body.len;
    while (n > 0 && (body.data[n - 1] == '\n' || body.data[n - 1] == '\r' ||
                     body.data[n - 1] == ' ' || body.data[n - 1] == '\t'))
        n--;
    body.len = n;
    if (note[0]) {
        buf_add(&body, "\n", 1);
        buf_add(&body, note, strlen(note));
    }

    snprintf(head, sizeof head, "/%s\n", def->name);
    buf_add(&msg, head, strlen(head));
    if (body.len == 0) {
        buf_add(&msg, "(no output)", 11);
    } else {
        cut = utf8_cut(body.data, body.len, OUT_MAX);
        buf_add(&msg, body.data, cut);
        if (cut < body.len)
            buf_add(&msg, "\n... (truncated)", 16);
    }
    send_text(chat, msg.data);
    buf_free(&body);
    buf_free(&msg);
}

static void cmd_commands(int64_t chat) {
    buf_t list = {0};
    char line[256];

    snprintf(line, sizeof line, "Commands (%d):\n", RCT_NCOMMANDS);
    buf_add(&list, line, strlen(line));
    for (int i = 0; i < RCT_NCOMMANDS; i++) {
        snprintf(line, sizeof line, "/%s - %s\n", RCT_COMMANDS[i].name, RCT_COMMANDS[i].desc);
        buf_add(&list, line, strlen(line));
    }
    send_long(chat, list.data);
    buf_free(&list);
}

#include "monitor.h"

static void handle_message(int64_t chat, const char *text) {
    char cmd[64];
    char arg[MAX_TEXT];
    char *at;
    time_t now = time(NULL);
    int idx = chat_index(chat);
    chat_state *c;

    if (idx < 0) {
        logf_("ignored message from unauthorised chat %" PRId64, chat);
        return;
    }
    c = &g_chats[idx];
    g_cmds_handled++;

    split_command(text, cmd, sizeof cmd, arg, sizeof arg);
    if (cmd[0] != '/')
        return;

    /* Groups send commands as /cmd@BotName. */
    at = strchr(cmd, '@');
    if (at)
        *at = '\0';

    logf_("command %s from chat %" PRId64, cmd, chat);

    if (strcmp(cmd, "/start") == 0 || strcmp(cmd, "/help") == 0) {
        cmd_help(chat);
        return;
    }
    if (strcmp(cmd, "/auth") == 0) {
        cmd_auth(c, arg, now);
        return;
    }
    if (strcmp(cmd, "/logout") == 0) {
        cmd_logout(c);
        return;
    }

    if (!session_active(c, now)) {
        reply(chat, "Not authenticated. Send /auth <password> first.");
        return;
    }
    c->session_until = now + g_session_s; /* sliding idle timeout */

    if (monitor_dispatch(chat, c, cmd, arg, now))
        return;

    if (strcmp(cmd, "/status") == 0) {
        cmd_status(chat);
    } else if (strcmp(cmd, "/ip") == 0) {
        cmd_ip(chat, arg);
    } else if (strcmp(cmd, "/commands") == 0) {
        cmd_commands(chat);
    } else {
        const rct_cmd_def *def = find_table_cmd(cmd + 1);

        if (def)
            run_table_command(chat, def);
        else
            reply(chat, "Unknown command. Send /help for the list.");
    }
}

/* ------------------------------------------------------------------------- */
/* Update processing                                                         */
/* ------------------------------------------------------------------------- */

static void handle_update(const char *u) {
    const char *msg;
    const char *chat_obj;
    const char *text_v;
    int64_t uid;
    int64_t chat_id;
    char text[MAX_TEXT];

    if (json_int(json_get(u, "update_id"), &uid) && uid >= g_offset)
        g_offset = uid + 1;

    msg = json_get(u, "message");
    if (!msg)
        return;
    chat_obj = json_get(msg, "chat");
    if (!chat_obj || !json_int(json_get(chat_obj, "id"), &chat_id))
        return;
    text_v = json_get(msg, "text");
    if (!text_v || !json_str(text_v, text, sizeof text))
        return;

    handle_message(chat_id, text);
}

/* Returns false if Telegram reported an error (the caller should back off). */
static bool handle_response(const char *json) {
    const char *ok = json_get(json, "ok");
    const char *result = json_get(json, "result");
    const char *p;

    if (!ok || strncmp(ok, "true", 4) != 0) {
        const char *desc = json_get(json, "description");
        char d[256] = "";

        if (desc)
            json_str(desc, d, sizeof d);
        logf_("Telegram returned an error: %s", d[0] ? d : "(no description)");
        return false;
    }
    if (!result || *result != '[')
        return true;

    p = result + 1;
    for (;;) {
        p = skip_ws(p);
        if (*p == ']' || *p == '\0')
            break;
        if (*p == '{')
            handle_update(p);
        p = skip_ws(json_skip(p));
        if (*p == ',')
            p++;
        else
            break;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Configuration and main loop                                               */
/* ------------------------------------------------------------------------- */

static void sleep_interruptible(unsigned seconds) {
    while (seconds-- > 0 && !g_stop)
        sleep(1);
}

static bool valid_token(const char *t) {
    size_t n = strlen(t);

    if (n < 10 || n >= MAX_TOKEN)
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = t[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') || c == ':' || c == '_' || c == '-'))
            return false;
    }
    return true;
}

static bool parse_long(const char *s, long *out) {
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0)
        return false;
    *out = v;
    return true;
}

static bool load_config(void) {
    const char *tok = getenv("SUPER_RCT_TOKEN");
    const char *chats = getenv("SUPER_RCT_CHAT_IDS");
    const char *pw = getenv("SUPER_RCT_PASSWORD");
    const char *sess = getenv("SUPER_RCT_SESSION_SECONDS");
    const char *api = getenv("SUPER_RCT_API");
    const char *ipf = getenv("SUPER_RCT_IP_FILE");
    const char *sf = getenv("SUPER_RCT_STATE_FILE");

    if (!tok || !valid_token(tok)) {
        fprintf(stderr, "error: SUPER_RCT_TOKEN is missing or malformed\n");
        return false;
    }
    strcpy(g_token, tok);

    {
        const char *sh = getenv("SUPER_RCT_SHELL");

        if (sh && *sh) {
            if (strcmp(sh, "0") == 0)
                g_shell_enabled = 0;
            else if (strcmp(sh, "1") != 0) {
                fprintf(stderr, "error: SUPER_RCT_SHELL must be 0 or 1 (default 1)\n");
                return false;
            }
        }
    }

    if (!pw || strlen(pw) < MIN_PASSWORD || strlen(pw) > MAX_PASSWORD) {
        fprintf(stderr, "error: SUPER_RCT_PASSWORD is required (%d-%d bytes)\n",
                MIN_PASSWORD, MAX_PASSWORD);
        return false;
    }
    strcpy(g_password, pw);

    if (sess && *sess) {
        long v;

        if (!parse_long(sess, &v) || v < MIN_SESSION_S || v > MAX_SESSION_S) {
            fprintf(stderr, "error: SUPER_RCT_SESSION_SECONDS must be %d-%d\n",
                    MIN_SESSION_S, MAX_SESSION_S);
            return false;
        }
        g_session_s = v;
    }

    if (!chats || !*chats) {
        fprintf(stderr, "error: SUPER_RCT_CHAT_IDS is required (comma-separated)\n");
        return false;
    }
    {
        const char *p = chats;
        char *end;

        for (;;) {
            long long v;

            while (*p == ' ' || *p == '\t')
                p++;
            v = strtoll(p, &end, 10);
            if (end == p || g_nchats >= MAX_CHATS) {
                fprintf(stderr, "error: bad chat id list in SUPER_RCT_CHAT_IDS\n");
                return false;
            }
            g_chats[g_nchats].chat = (int64_t)v;
            g_nchats++;
            p = end;
            while (*p == ' ' || *p == '\t')
                p++;
            if (*p == '\0')
                break;
            if (*p != ',') {
                fprintf(stderr, "error: bad chat id list in SUPER_RCT_CHAT_IDS\n");
                return false;
            }
            p++;
        }
    }

    {
        const char *dt = getenv("SUPER_RCT_DISCORD_TOKEN");
        const char *dc = getenv("SUPER_RCT_DISCORD_CHANNEL_IDS");
        const char *da = getenv("SUPER_RCT_DISCORD_API");

        if (da && *da) {
            if (strlen(da) >= MAX_URL) {
                fprintf(stderr, "error: SUPER_RCT_DISCORD_API is too long\n");
                return false;
            }
            if (strncmp(da, "https://", 8) != 0 && strncmp(da, "http://", 7) != 0) {
                fprintf(stderr, "error: SUPER_RCT_DISCORD_API must start with https:// or http://\n");
                return false;
            }
            snprintf(g_discord_api, sizeof g_discord_api, "%s", da);
        }
        if (dt && *dt) {
            const char *p = dc ? dc : "";
            char *end;

            if (!discord_token_valid(dt)) {
                fprintf(stderr, "error: SUPER_RCT_DISCORD_TOKEN is malformed\n");
                return false;
            }
            if (!*p) {
                fprintf(stderr, "error: SUPER_RCT_DISCORD_CHANNEL_IDS is required with a Discord token\n");
                return false;
            }
            snprintf(g_discord_token, sizeof g_discord_token, "%s", dt);
            g_discord_on = true;
            for (;;) {
                long long v;

                while (*p == ' ' || *p == '\t')
                    p++;
                v = strtoll(p, &end, 10);
                if (end == p || v <= 0 || g_nchats >= MAX_CHATS) {
                    fprintf(stderr, "error: bad channel id list in SUPER_RCT_DISCORD_CHANNEL_IDS\n");
                    return false;
                }
                for (int i = 0; i < g_nchats; i++)
                    if (g_chats[i].chat == (int64_t)v) {
                        fprintf(stderr, "error: an ID appears in both chat and channel lists\n");
                        return false;
                    }
                g_chats[g_nchats].chat = (int64_t)v;
                g_chats[g_nchats].discord = true;
                g_nchats++;
                p = end;
                while (*p == ' ' || *p == '\t')
                    p++;
                if (*p == '\0')
                    break;
                if (*p != ',') {
                    fprintf(stderr, "error: bad channel id list in SUPER_RCT_DISCORD_CHANNEL_IDS\n");
                    return false;
                }
                p++;
            }
        }
    }

    for (int i = 0; i < g_nchats; i++)
        for (int k = 0; k < 4; k++)
            g_chats[i].thr[k] = -1.0;
    for (int i = 0; i < g_nchats; i++) {
        g_chats[i].quiet_start = -1;
        g_chats[i].quiet_end = -1;
        g_chats[i].daily_at = -1;
    }

    {
        const char *wp = getenv("SUPER_RCT_WATCH_PATHS");
        const char *wa = getenv("SUPER_RCT_WATCH_MAX_AGE_HOURS");

        if (wp && *wp) {
            char *copy = strdup(wp);
            char *save = NULL;
            char *wtok;

            if (!copy)
                return false;
            for (wtok = strtok_r(copy, ",", &save); wtok; wtok = strtok_r(NULL, ",", &save)) {
                size_t len;

                while (*wtok == ' ')
                    wtok++;
                len = strlen(wtok);
                while (len > 0 && wtok[len - 1] == ' ')
                    wtok[--len] = '\0';
                if (len == 0)
                    continue;
                if (wtok[0] != '/' || len >= MAX_URL || g_nwatch >= MAX_WATCH) {
                    fprintf(stderr, "error: SUPER_RCT_WATCH_PATHS needs absolute paths (max %d)\n",
                            MAX_WATCH);
                    free(copy);
                    return false;
                }
                snprintf(g_watch[g_nwatch], MAX_URL, "%s", wtok);
                g_nwatch++;
            }
            free(copy);
        }
        if (wa && *wa) {
            long v;

            if (!parse_long(wa, &v) || v < 1 || v > 8760) {
                fprintf(stderr, "error: SUPER_RCT_WATCH_MAX_AGE_HOURS must be 1-8760\n");
                return false;
            }
            g_watch_max_h = v;
        }
    }

    if (api && *api) {
        if (strncmp(api, "https://", 8) != 0 && strncmp(api, "http://", 7) != 0) {
            fprintf(stderr, "error: SUPER_RCT_API must start with https:// or http://\n");
            return false;
        }
        if (strlen(api) >= sizeof g_api) {
            fprintf(stderr, "error: SUPER_RCT_API is too long\n");
            return false;
        }
        strcpy(g_api, api);
    }
    if (ipf && *ipf) {
        if (strlen(ipf) >= sizeof g_ipfile) {
            fprintf(stderr, "error: SUPER_RCT_IP_FILE is too long\n");
            return false;
        }
        strcpy(g_ipfile, ipf);
    }
    if (sf && *sf) {
        if (strlen(sf) >= sizeof g_statefile) {
            fprintf(stderr, "error: SUPER_RCT_STATE_FILE is too long\n");
            return false;
        }
        strcpy(g_statefile, sf);
    }
    load_state(); /* after the path is known */
    return true;
}

static void usage(FILE *out) {
    fprintf(out,
            "Super_RCT %s - Telegram remote-control bot\n\n"
            "Usage: super_rct [--help] [--version]\n\n"
            "Environment:\n"
            "  SUPER_RCT_TOKEN            bot token (required)\n"
            "  SUPER_RCT_CHAT_IDS         allowed chat IDs, comma-separated (required)\n"
            "  SUPER_RCT_PASSWORD         login password, 8-128 bytes (required)\n"
            "  SUPER_RCT_SESSION_SECONDS  idle timeout for a login (default %d)\n"
            "  SUPER_RCT_IP_FILE          IP registry file (default %s)\n"
            "  SUPER_RCT_WATCH_PATHS      comma-separated paths to watch (optional)\n"
            "  SUPER_RCT_WATCH_MAX_AGE_HOURS  watched file is stale after N hours (default 24)\n"
            "  SUPER_RCT_API              API base URL (default %s)\n",
            SRCT_VERSION, DEFAULT_SESSION_S, DEFAULT_IP_FILE, DEFAULT_API);
}

int main(int argc, char **argv) {
    struct sigaction sa;

    if (argc > 1) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(argv[1], "--version") == 0) {
            puts("super_rct " SRCT_VERSION);
            return 0;
        }
        usage(stderr);
        return 2;
    }

    if (!load_config()) {
        usage(stderr);
        return 2;
    }

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    g_started = time(NULL);
    ip_load();
    logf_("Super_RCT %s started (%d allowed chat(s), %d IP entries)", SRCT_VERSION,
          g_nchats, g_nips);

    while (!g_stop) {
        buf_t resp = {0};

        if (tg_get_updates(g_discord_on ? 1 : POLL_TIMEOUT_S, &resp) != 0 || !resp.data) {
            if (g_stop) {
                buf_free(&resp);
                break;
            }
            logf_("getUpdates failed; retrying in %d s", RETRY_DELAY_S);
            buf_free(&resp);
            sleep_interruptible(RETRY_DELAY_S);
            continue;
        }
        if (!handle_response(resp.data))
            sleep_interruptible(RETRY_DELAY_S); /* back off on API errors */
        buf_free(&resp);
        if (g_discord_on && time(NULL) >= g_next_discord_poll) {
            g_next_discord_poll = time(NULL) + DISCORD_POLL_S;
            for (int i = 0; i < g_nchats; i++)
                if (g_chats[i].discord)
                    discord_poll(&g_chats[i], handle_message);
        }
        monitor_tick(time(NULL));
    }

    logf_("Super_RCT stopped");
    return 0;
}
