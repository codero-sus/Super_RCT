/*
 * Super_RCT installer for Linux (C build).
 *
 *   ./super_rct-linux install [--no-service]   extract the bot here, ask for
 *                                              settings, register auto-start
 *   ./super_rct-linux uninstall                stop auto-start, remove what
 *                                              install created
 *
 * The bot binary is embedded below (base64) and extracted into the current
 * directory. Only the systemd user service in ~/.config/systemd/user is written
 * outside it. This file is never deleted. Secrets are typed without echo when
 * a terminal is attached; otherwise they are read from stdin.
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define BOT_NAME "super_rct"
#define ENV_NAME "super_rct.env"
#define MANIFEST "super_rct_install.txt"
/* Data the bot writes into its directory at run time; uninstall removes these too. */
static const char *const RUNTIME_FILES[] = {
    "super_rct_state.txt", "super_rct_state.txt.tmp",
    "super_rct_ips.txt", "super_rct_ips.txt.tmp",
};
#define UNIT_NAME "super-rct.service"
#define PATHBUF (PATH_MAX * 2)

/* Replaced by tools/build_installers.py with the base64 bot binary, one entry per chunk. */
static const char *const PAYLOAD_PARTS[] = {""};
#define PAYLOAD_PARTS_N (sizeof PAYLOAD_PARTS / sizeof PAYLOAD_PARTS[0])

static void die(const char *msg) {
    fprintf(stderr, "error: %s\n", msg);
    exit(1);
}

static void trim(char *s) {
    size_t n = strlen(s);

    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
    size_t lead = strspn(s, " \t");
    if (lead)
        memmove(s, s + lead, strlen(s + lead) + 1);
}

/* Reads one setting. Secrets use getpass() when stdin is a terminal. */
static void read_value(const char *label, char *buf, size_t n, bool secret, bool required) {
    for (;;) {
        if (secret && isatty(STDIN_FILENO)) {
            const char *p = getpass(label);

            if (!p)
                die("no input");
            snprintf(buf, n, "%s", p);
        } else {
            fprintf(stderr, "%s", label);
            fflush(stderr);
            if (!fgets(buf, (int)n, stdin))
                die("no input");
        }
        trim(buf);
        if (buf[0] || !required)
            return;
        fprintf(stderr, "  (required)\n");
    }
}

static int b64val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decodes the payload parts in order, as one base64 stream. */
static unsigned char *b64_decode_parts(size_t *out_len) {
    size_t total = 0;
    unsigned char *out;
    unsigned acc = 0;
    int bits = 0;
    size_t o = 0;

    for (size_t k = 0; k < PAYLOAD_PARTS_N; k++)
        total += strlen(PAYLOAD_PARTS[k]);
    out = malloc(total / 4 * 3 + 3);
    if (!out)
        die("out of memory");
    for (size_t k = 0; k < PAYLOAD_PARTS_N; k++) {
        const char *in = PAYLOAD_PARTS[k];

        for (size_t i = 0; in[i]; i++) {
            int v = b64val((unsigned char)in[i]);

            if (v < 0)
                continue;
            acc = ((acc << 6) | (unsigned)v) & 0xFFFFu;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out[o++] = (unsigned char)((acc >> bits) & 0xFF);
            }
        }
    }
    *out_len = o;
    return out;
}

/* Joins a directory and a file name, refusing to truncate. */
static void join_path(char *dst, size_t n, const char *dir, const char *name) {
    if (snprintf(dst, n, "%s/%s", dir, name) >= (int)n)
        die("path too long");
}

static bool exists(const char *path) {
    struct stat st;

    return lstat(path, &st) == 0;
}

static void write_all(int fd, const void *data, size_t n) {
    const char *p = data;

    while (n > 0) {
        ssize_t w = write(fd, p, n);

        if (w < 0) {
            if (errno == EINTR)
                continue;
            die("write failed");
        }
        p += w;
        n -= (size_t)w;
    }
}

static void write_file(const char *path, const void *data, size_t n, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);

    if (fd < 0) {
        fprintf(stderr, "error: cannot write %s: %s\n", path, strerror(errno));
        exit(1);
    }
    write_all(fd, data, n);
    close(fd);
}

/* Runs a program without a shell. Returns its exit status, or -1. */
static int run(char *const argv[]) {
    pid_t pid = fork();
    int status = 0;

    if (pid < 0)
        return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void mkdirs(const char *path) {
    char tmp[PATHBUF];

    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
                die("cannot create a directory under HOME");
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        die("cannot create the systemd user directory");
}

/* systemd quoting: double quotes, with backslash escapes for \ and ". */
static void unit_quote(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '\\' || *s == '"')
            fputc('\\', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

static int unit_path(char *out, size_t n) {
    const char *home = getenv("HOME");

    if (!home || !*home)
        die("HOME is not set; cannot find the systemd user directory");
    snprintf(out, n, "%s/.config/systemd/user/%s", home, UNIT_NAME);
    return 0;
}

static int install(bool no_service) {
    char cwd[PATHBUF];
    char env_path[PATHBUF];
    char bin_path[PATHBUF];
    char token[256], chats[512], pw[256], pw2[256], dtoken[256], dchats[512];
    char unit[PATHBUF];
    size_t bin_len = 0;
    unsigned char *bin;
    FILE *env;
    int fd;

    if (exists(MANIFEST)) {
        printf("Already installed here (%s exists). Run uninstall first.\n", MANIFEST);
        return 1;
    }
    if (!PAYLOAD_PARTS[0][0])
        die("this installer has no bot payload; build it with tools/build_installers.py");
    if (!getcwd(cwd, sizeof cwd))
        die("cannot read the current directory");
    if (exists(BOT_NAME) || exists(ENV_NAME))
        die("refusing to overwrite existing " BOT_NAME " or " ENV_NAME);

    fprintf(stderr, "Super_RCT setup. Secrets are saved to %s (readable only by you).\n", ENV_NAME);
    read_value("Telegram bot token: ", token, sizeof token, true, true);
    read_value("Allowed Telegram chat IDs (comma-separated): ", chats, sizeof chats, false, true);
    for (;;) {
        read_value("Login password (8-128 characters): ", pw, sizeof pw, true, true);
        read_value("Repeat password: ", pw2, sizeof pw2, true, true);
        if (strcmp(pw, pw2) == 0)
            break;
        fprintf(stderr, "  Passwords differ, try again.\n");
    }
    read_value("Discord bot token (Enter to skip): ", dtoken, sizeof dtoken, true, false);
    dchats[0] = '\0';
    if (dtoken[0])
        read_value("Allowed Discord channel IDs: ", dchats, sizeof dchats, false, true);

    if (strchr(token, '\n') || strchr(pw, '\n') || strchr(dtoken, '\n'))
        die("values must not contain line breaks");

    /* Extract the bot binary. */
    bin = b64_decode_parts(&bin_len);
    join_path(bin_path, sizeof bin_path, cwd, BOT_NAME);
    write_file(bin_path, bin, bin_len, 0755);
    free(bin);

    /* Settings file, readable only by the owner. */
    join_path(env_path, sizeof env_path, cwd, ENV_NAME);
    fd = open(env_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        die("cannot write the settings file");
    env = fdopen(fd, "w");
    if (!env)
        die("cannot write the settings file");
    fprintf(env, "SUPER_RCT_TOKEN=%s\n", token);
    fprintf(env, "SUPER_RCT_CHAT_IDS=%s\n", chats);
    fprintf(env, "SUPER_RCT_PASSWORD=%s\n", pw);
    if (dtoken[0]) {
        fprintf(env, "SUPER_RCT_DISCORD_TOKEN=%s\n", dtoken);
        fprintf(env, "SUPER_RCT_DISCORD_CHANNEL_IDS=%s\n", dchats);
    }
    fclose(env);

    printf("Extracted %s and wrote %s.\n", BOT_NAME, ENV_NAME);

    unit[0] = '\0';
    if (!no_service) {
        char dir[PATHBUF];
        FILE *f;
        char *const reload[] = {"systemctl", "--user", "daemon-reload", NULL};
        char *const enable[] = {"systemctl", "--user", "enable", "--now", UNIT_NAME, NULL};

        unit_path(unit, sizeof unit);
        snprintf(dir, sizeof dir, "%s", unit);
        *strrchr(dir, '/') = '\0';
        mkdirs(dir);
        f = fopen(unit, "w");
        if (!f)
            die("cannot write the service file");
        fprintf(f, "[Unit]\nDescription=Super_RCT bot\nAfter=network-online.target\n\n[Service]\n");
        fprintf(f, "WorkingDirectory=");
        unit_quote(f, cwd);
        fprintf(f, "\nEnvironmentFile=");
        unit_quote(f, env_path);
        fprintf(f, "\nExecStart=");
        unit_quote(f, bin_path);
        fprintf(f, "\nRestart=on-failure\nRestartSec=10\n\n[Install]\nWantedBy=default.target\n");
        fclose(f);
        if (run(reload) != 0 || run(enable) != 0)
            fprintf(stderr,
                    "warning: could not enable the systemd user service here. The bot files are "
                    "in place; start %s with the settings in %s.\n",
                    BOT_NAME, ENV_NAME);
        printf("Service file written: %s\n", unit);
    }

    /* Record what was created so uninstall can remove exactly that. */
    {
        FILE *m = fopen(MANIFEST, "w");

        if (!m)
            die("cannot write " MANIFEST);
        fprintf(m, "file %s\nfile %s\n", BOT_NAME, ENV_NAME);
        if (unit[0])
            fprintf(m, "unit %s\n", unit);
        fclose(m);
    }
    printf("Done. To remove everything later: ./%s uninstall\n", BOT_NAME);
    return 0;
}

static int uninstall(void) {
    FILE *m = fopen(MANIFEST, "r");
    char line[PATHBUF];

    if (!m) {
        printf("Nothing to uninstall here (no %s).\n", MANIFEST);
        return 0;
    }
    while (fgets(line, sizeof line, m)) {
        char *nl = strchr(line, '\n');
        char *arg;

        if (nl)
            *nl = '\0';
        arg = strchr(line, ' ');
        if (!arg)
            continue;
        *arg++ = '\0';
        if (strcmp(line, "unit") == 0 && exists(arg)) {
            char *const disable[] = {"systemctl", "--user", "disable", "--now", UNIT_NAME, NULL};
            char *const reload[] = {"systemctl", "--user", "daemon-reload", NULL};

            run(disable);
            unlink(arg);
            run(reload);
            printf("Removed service %s\n", arg);
        }
    }
    rewind(m);
    while (fgets(line, sizeof line, m)) {
        char *nl = strchr(line, '\n');
        char *arg;

        if (nl)
            *nl = '\0';
        arg = strchr(line, ' ');
        if (!arg)
            continue;
        *arg++ = '\0';
        if (strcmp(line, "file") == 0 && unlink(arg) == 0)
            printf("Removed %s\n", arg);
    }
    fclose(m);
    for (size_t i = 0; i < sizeof RUNTIME_FILES / sizeof RUNTIME_FILES[0]; i++) {
        if (unlink(RUNTIME_FILES[i]) == 0)
            printf("Removed %s (bot data)\n", RUNTIME_FILES[i]);
    }
    unlink(MANIFEST);
    printf("Uninstalled. This installer file was left in place.\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "install") == 0)
        return install(false);
    if (argc == 3 && strcmp(argv[1], "install") == 0 && strcmp(argv[2], "--no-service") == 0)
        return install(true);
    if (argc == 2 && strcmp(argv[1], "uninstall") == 0)
        return uninstall();
    fprintf(stderr, "usage: %s install [--no-service] | uninstall\n", argv[0]);
    return 2;
}
