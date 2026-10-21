/*
 * discord_transport.h - Discord transport. Included by super_rct.c after the
 * curl and JSON helpers.
 *
 * Polling over the REST API, so there is no gateway connection. Requests go
 * through the same curl helper as Telegram, so the token is sent on curl's
 * stdin config and never appears on the command line. The bot needs the
 * "Message Content" privileged intent, or message text arrives empty.
 */

#define DISCORD_MAX_MSG 1900 /* Discord allows 2000 characters */
#define DISCORD_POLL_S 3
#define DISCORD_BATCH 50
#define DISCORD_UA "DiscordBot (https://github.com/codero-sus/Super_RCT, 0.5.0)"

typedef struct {
    uint64_t id;
    char text[MAX_TEXT];
} discord_msg;

static discord_msg g_dmsgs[DISCORD_BATCH];

static bool discord_token_valid(const char *t) {
    size_t n = strlen(t);

    if (n < 30 || n > 128)
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = t[i];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              c == '.' || c == '_' || c == '-'))
            return false;
    }
    return true;
}

/* Appends s[0..n) as the body of a JSON string, escaping as needed. */
static void json_escape_add(buf_t *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        switch (c) {
        case '"':  buf_add(b, "\\\"", 2); break;
        case '\\': buf_add(b, "\\\\", 2); break;
        case '\n': buf_add(b, "\\n", 2); break;
        case '\r': buf_add(b, "\\r", 2); break;
        case '\t': buf_add(b, "\\t", 2); break;
        default:
            if (c < 0x20) {
                char u[8];

                snprintf(u, sizeof u, "\\u%04x", c);
                buf_add(b, u, 6);
            } else {
                buf_add(b, s + i, 1);
            }
            break;
        }
    }
}

/* One REST call. `body` is an optional JSON document. Retries once on a 429. */
static int discord_call(const char *method, const char *path, const char *body, buf_t *out) {
    char url[MAX_URL + 160];
    char auth[MAX_TOKEN + 32];
    int rc = -1;

    snprintf(url, sizeof url, "%s%s", g_discord_api, path);
    snprintf(auth, sizeof auth, "Authorization: Bot %s", g_discord_token);
    for (int attempt = 0; attempt < 2; attempt++) {
        buf_t cfg = {0};
        const char *wait;

        cfg_put(&cfg, "url", url);
        cfg_put(&cfg, "request", method);
        cfg_put(&cfg, "header", auth);
        cfg_put(&cfg, "header", "Content-Type: application/json");
        cfg_put(&cfg, "user-agent", DISCORD_UA);
        cfg_put(&cfg, "max-time", "20");
        if (body)
            cfg_put(&cfg, "data-binary", body);
        buf_free(out);
        rc = run_curl(&cfg, out);
        buf_free(&cfg);

        wait = out->data ? json_get(out->data, "retry_after") : NULL;
        if (!wait || attempt == 1)
            break;
        {
            double secs = strtod(wait, NULL);

            if (secs < 0)
                secs = 0;
            if (secs > 30)
                secs = 30;
            plat_sleep_ms(((int)secs + 1) * 1000);
        }
    }
    return rc;
}

/* Sends `text` to a channel, split into messages Discord accepts. */
static void discord_send(int64_t channel, const char *text) {
    char path[64];
    size_t len;
    size_t off = 0;

    if (!*text)
        text = "(no output)";
    len = strlen(text);
    snprintf(path, sizeof path, "/channels/%" PRId64 "/messages", channel);
    while (off < len) {
        size_t n = len - off;
        buf_t body = {0};
        buf_t out = {0};
        int rc;

        if (n > DISCORD_MAX_MSG) {
            n = DISCORD_MAX_MSG;
            /* Never cut inside a UTF-8 character. */
            while (n > 0 && (((unsigned char)text[off + n]) & 0xC0) == 0x80)
                n--;
        }
        buf_add(&body, "{\"content\":\"", 12);
        json_escape_add(&body, text + off, n);
        buf_add(&body, "\"}", 2);

        rc = discord_call("POST", path, body.data, &out);
        if (rc != 0)
            logf_("Discord send to channel %" PRId64 " failed", channel);
        buf_free(&body);
        buf_free(&out);
        off += n;
    }
}

/* Reads a snowflake ID, which Discord sends as a JSON string. */
static bool discord_id(const char *v, uint64_t *out) {
    char s[32];
    char *end;

    if (!v || !json_str(v, s, sizeof s))
        return false;
    *out = strtoull(s, &end, 10);
    return end != s && *end == '\0';
}

/*
 * Fetches new human messages for one channel and passes them to `handler`,
 * oldest first. The first successful call only records the newest message,
 * so history is never replayed as commands.
 */
static void discord_poll(chat_state *c, void (*handler)(int64_t, const char *)) {
    char path[160];
    buf_t out = {0};
    const char *p;
    int n = 0;

    if (!c->dc_primed) {
        snprintf(path, sizeof path, "/channels/%" PRId64 "/messages?limit=1", c->chat);
        if (discord_call("GET", path, NULL, &out) != 0 || !out.data)
            goto done;
        p = skip_ws(out.data);
        if (*p != '[')
            goto done;
        p = skip_ws(p + 1);
        if (*p == ']') {
            c->dc_cursor = 0;
        } else {
            uint64_t id;

            if (*p != '{' || !discord_id(json_get(p, "id"), &id))
                goto done;
            c->dc_cursor = id;
        }
        c->dc_primed = true;
        goto done;
    }

    snprintf(path, sizeof path, "/channels/%" PRId64 "/messages?after=%" PRIu64 "&limit=%d",
             c->chat, c->dc_cursor, DISCORD_BATCH);
    if (discord_call("GET", path, NULL, &out) != 0 || !out.data)
        goto done;
    p = skip_ws(out.data);
    if (*p != '[')
        goto done;
    p++;
    for (;;) {
        uint64_t id;

        p = skip_ws(p);
        if (*p != '{')
            break;
        if (discord_id(json_get(p, "id"), &id)) {
            const char *author = json_get(p, "author");
            const char *bot = author ? json_get(author, "bot") : NULL;
            const char *content = json_get(p, "content");

            if (id > c->dc_cursor)
                c->dc_cursor = id;
            if (n < DISCORD_BATCH && !(bot && strncmp(bot, "true", 4) == 0)) {
                g_dmsgs[n].id = id;
                g_dmsgs[n].text[0] = '\0';
                if (content)
                    json_str(content, g_dmsgs[n].text, sizeof g_dmsgs[n].text);
                n++;
            }
        }
        p = skip_ws(json_skip(p));
        if (*p != ',')
            break;
        p++;
    }

done:
    buf_free(&out);
    /* Insertion sort by message ID, so commands run in the order they were sent. */
    for (int i = 1; i < n; i++) {
        discord_msg tmp = g_dmsgs[i];
        int j = i - 1;

        while (j >= 0 && g_dmsgs[j].id > tmp.id) {
            g_dmsgs[j + 1] = g_dmsgs[j];
            j--;
        }
        g_dmsgs[j + 1] = tmp;
    }
    for (int i = 0; i < n; i++)
        handler(c->chat, g_dmsgs[i].text);
}
