/*
 * Super_RCT - a small Telegram remote-control bot (C variant).
 *
 * Features
 *   - Long-polls the Telegram Bot API with getUpdates.
 *   - Only chats listed in SUPER_RCT_CHAT_IDS may issue commands.
 *   - Built-in commands: /help, /status, /ip list|enable|disable.
 *   - Keeps an IP registry (enabled/disabled) in a plain text file.
 *
 * Design notes
 *   - POSIX only (Linux, macOS, BSD). No runtime libraries besides libc.
 *   - HTTPS is delegated to the system `curl` executable. This avoids the
 *     need for libcurl development headers. Requests are passed to curl
 *     through its config-file interface on stdin, so the bot token never
 *     appears in the process list.
 *   - JSON is parsed with a small depth-aware reader that only understands
 *     the subset of fields the bot needs.
 *   - No arbitrary shell execution is offered, by design.
 *
 * Configuration (environment variables)
 *   SUPER_RCT_TOKEN      Telegram bot token (required)
 *   SUPER_RCT_CHAT_IDS   Comma-separated list of allowed chat IDs (required)
 *   SUPER_RCT_IP_FILE    IP registry file (default: super_rct_ips.txt)
 *   SUPER_RCT_API        API base URL (default: https://api.telegram.org)
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
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

#define SRCT_VERSION "0.2.0"

#define DEFAULT_API "https://api.telegram.org"
#define DEFAULT_IP_FILE "super_rct_ips.txt"
#define MAX_TOKEN 256
#define MAX_URL 512
#define MAX_CHATS 32
#define MAX_IPS 256
#define MAX_TEXT 1024
#define IP_LEN 64
#define POLL_TIMEOUT_S 25
#define HTTP_MAX_TIME_S 40
#define RETRY_DELAY_S 5

/* ------------------------------------------------------------------------- */
/* Global state                                                              */
/* ------------------------------------------------------------------------- */

typedef struct {
    char ip[IP_LEN];
    bool enabled;
} ip_entry;

static volatile sig_atomic_t g_stop = 0;
static time_t g_started;
static char g_token[MAX_TOKEN];
static char g_api[MAX_URL] = DEFAULT_API;
static char g_ipfile[MAX_URL] = DEFAULT_IP_FILE;
static int64_t g_chats[MAX_CHATS];
static int g_nchats;
static int64_t g_offset;
static ip_entry g_ips[MAX_IPS];
static int g_nips;

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

static int tg_get_updates(buf_t *out) {
    buf_t cfg = {0};
    char url[MAX_URL + MAX_TOKEN + 64];
    char kv[64];
    int rc;

    api_url(url, sizeof url, "getUpdates");
    cfg_put(&cfg, "url", url);
    /* One data-urlencode directive per field, so each is sent as its own parameter. */
    snprintf(kv, sizeof kv, "timeout=%d", POLL_TIMEOUT_S);
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
    buf_t out = {0};
    char url[MAX_URL + MAX_TOKEN + 64];
    char kv[MAX_TEXT + 64];
    int rc;

    api_url(url, sizeof url, "sendMessage");
    cfg_put(&cfg, "url", url);
    snprintf(kv, sizeof kv, "chat_id=%" PRId64, chat);
    cfg_put(&cfg, "data-urlencode", kv);
    snprintf(kv, sizeof kv, "text=%s", text);
    cfg_put(&cfg, "data-urlencode", kv);
    cfg_put(&cfg, "max-time", "20");

    rc = run_curl(&cfg, &out);
    if (rc != 0)
        logf_("sendMessage to chat %" PRId64 " failed", chat);
    buf_free(&cfg);
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

    if (inet_pton(AF_INET, in, buf) == 1)
        return inet_ntop(AF_INET, buf, out, (socklen_t)n) != NULL;
    if (inet_pton(AF_INET6, in, buf) == 1)
        return inet_ntop(AF_INET6, buf, out, (socklen_t)n) != NULL;
    return false;
}

/* ------------------------------------------------------------------------- */
/* Command handlers                                                          */
/* ------------------------------------------------------------------------- */

static bool chat_allowed(int64_t chat) {
    for (int i = 0; i < g_nchats; i++)
        if (g_chats[i] == chat)
            return true;
    return false;
}

static void reply(int64_t chat, const char *fmt, ...) {
    char text[MAX_TEXT];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    tg_send_text(chat, text);
}

static void cmd_help(int64_t chat) {
    reply(chat,
          "Super_RCT %s commands:\n"
          "/status - host status\n"
          "/ip list - show IP registry\n"
          "/ip enable <ip> - mark an IP as enabled\n"
          "/ip disable <ip> - mark an IP as disabled\n"
          "/help - this message\n"
          "Note: the IP registry is informational. No firewall rules are changed.",
          SRCT_VERSION);
}

static void cmd_status(int64_t chat) {
    char host[256] = "unknown";
    long up = (long)difftime(time(NULL), g_started);

    gethostname(host, sizeof host - 1);
    reply(chat, "Super_RCT %s\nHost: %s\nPID: %ld\nUptime: %ldh %ldm %lds\nIP entries: %d",
          SRCT_VERSION, host, (long)getpid(), up / 3600, (up % 3600) / 60, up % 60,
          g_nips);
}

static void cmd_ip(int64_t chat, const char *sub, const char *arg) {
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

        if (arg[0] == '\0') {
            reply(chat, "Usage: /ip %s <ip>", sub);
            return;
        }
        if (!ip_normalise(arg, ip, sizeof ip)) {
            reply(chat, "Invalid IP address: %s", arg);
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

static void handle_message(int64_t chat, const char *text) {
    char cmd[64] = "";
    char sub[64] = "";
    char arg[128] = "";
    char *at;

    if (!chat_allowed(chat)) {
        logf_("ignored message from unauthorised chat %" PRId64, chat);
        return;
    }
    if (sscanf(text, " %63s %63s %127s", cmd, sub, arg) < 1 || cmd[0] != '/')
        return;

    /* Groups send commands as /cmd@BotName. */
    at = strchr(cmd, '@');
    if (at)
        *at = '\0';

    logf_("command %s from chat %" PRId64, cmd, chat);

    if (strcmp(cmd, "/start") == 0 || strcmp(cmd, "/help") == 0)
        cmd_help(chat);
    else if (strcmp(cmd, "/status") == 0)
        cmd_status(chat);
    else if (strcmp(cmd, "/ip") == 0)
        cmd_ip(chat, sub, arg);
    else
        reply(chat, "Unknown command. Send /help for the list.");
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

static void handle_response(const char *json) {
    const char *ok = json_get(json, "ok");
    const char *result = json_get(json, "result");
    const char *p;

    if (!ok || strncmp(ok, "true", 4) != 0) {
        const char *desc = json_get(json, "description");
        char d[256] = "";

        if (desc)
            json_str(desc, d, sizeof d);
        logf_("Telegram returned an error: %s", d[0] ? d : "(no description)");
        return;
    }
    if (!result || *result != '[')
        return;

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

static bool load_config(void) {
    const char *tok = getenv("SUPER_RCT_TOKEN");
    const char *chats = getenv("SUPER_RCT_CHAT_IDS");
    const char *api = getenv("SUPER_RCT_API");
    const char *ipf = getenv("SUPER_RCT_IP_FILE");

    if (!tok || !valid_token(tok)) {
        fprintf(stderr, "error: SUPER_RCT_TOKEN is missing or malformed\n");
        return false;
    }
    strcpy(g_token, tok);

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
            g_chats[g_nchats++] = (int64_t)v;
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
    return true;
}

static void usage(FILE *out) {
    fprintf(out,
            "Super_RCT %s - Telegram remote-control bot\n\n"
            "Usage: super_rct [--help] [--version]\n\n"
            "Environment:\n"
            "  SUPER_RCT_TOKEN      bot token (required)\n"
            "  SUPER_RCT_CHAT_IDS   allowed chat IDs, comma-separated (required)\n"
            "  SUPER_RCT_IP_FILE    IP registry file (default %s)\n"
            "  SUPER_RCT_API        API base URL (default %s)\n",
            SRCT_VERSION, DEFAULT_IP_FILE, DEFAULT_API);
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

        if (tg_get_updates(&resp) != 0 || !resp.data) {
            if (g_stop) {
                buf_free(&resp);
                break;
            }
            logf_("getUpdates failed; retrying in %d s", RETRY_DELAY_S);
            buf_free(&resp);
            sleep_interruptible(RETRY_DELAY_S);
            continue;
        }
        handle_response(resp.data);
        buf_free(&resp);
    }

    logf_("Super_RCT stopped");
    return 0;
}
