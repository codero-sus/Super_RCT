/*
 * platform.h - the operating-system calls the bot needs.
 *
 * POSIX systems (Linux, macOS, BSD) use fork, pipes and process groups.
 * Windows uses CreateProcess, pipes and a job object, so a timeout or a stop
 * kills the whole process tree. Included by super_rct.c before any other
 * system-specific code.
 */

#ifndef SRCT_PLATFORM_H
#define SRCT_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#define PLAT_EXIT_NOT_FOUND 127 /* the program could not be started */
#define PLAT_EXIT_TIMEOUT 124   /* killed: timeout or stop */

/* Receives the child's stdout and stderr as they arrive. */
typedef void (*plat_sink_fn)(void *ctx, const char *data, size_t n);

typedef struct {
    char *const *argv;   /* program and arguments, no shell (used when cmd is NULL) */
    const char *cmd;     /* shell command line: sh -c on POSIX, cmd /d /s /c on Windows */
    const char *in;      /* bytes for the child's stdin, or NULL for an empty stdin */
    size_t inlen;
    int timeout_s;       /* 0 = no limit */
    plat_sink_fn sink;
    void *ctx;
    const volatile sig_atomic_t *stop; /* when this becomes non-zero, the child is killed */
    bool missing_note;   /* write "command not available on this host" if it cannot start */
} plat_run_t;

/* Runs a child and returns its exit status (0-255), PLAT_EXIT_TIMEOUT if it was
 * killed, PLAT_EXIT_NOT_FOUND if it could not start, or -1 on a local error. */
int plat_run(const plat_run_t *r);
/* Seconds from an arbitrary fixed point; never goes backwards. */
double plat_mono(void);
void plat_sleep_ms(int ms);
/* Local time. Returns false if the time cannot be represented. */
bool plat_localtime(time_t t, struct tm *out);
bool plat_hostname(char *buf, size_t n);
long plat_pid(void);
/* Calls handler(signal) for Ctrl+C and termination requests. Ignores broken pipes. */
void plat_catch_stop(void (*handler)(int));
/* Replaces `to` with `from` atomically (overwrites an existing file on Windows too). */
bool plat_replace_file(const char *from, const char *to);
/* Flushes a stdio stream to disk. */
bool plat_fsync(FILE *f);

#define PLAT_MISSING_TEXT "command not available on this host\n"

#if defined(_WIN32)

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 /* Windows 7: inet_pton and inet_ntop */
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#ifndef F_OK
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#endif

static bool plat_win_timed_out(double deadline) {
    return deadline > 0 && plat_mono() >= deadline;
}

static void plat_win_append_arg(char **buf, size_t *len, size_t *cap, const char *arg) {
    /* Quotes an argument for CreateProcess: wrap in quotes when it has spaces. */
    bool quote = arg[0] == '\0' || strpbrk(arg, " \t\"") != NULL;
    size_t need = strlen(arg) * 2 + 4;

    if (*len + need + 2 > *cap) {
        size_t next = (*cap + need + 2) * 2;
        char *grown = realloc(*buf, next);

        if (!grown)
            return;
        *buf = grown;
        *cap = next;
    }
    if (*len > 0)
        (*buf)[(*len)++] = ' ';
    if (quote)
        (*buf)[(*len)++] = '"';
    for (const char *p = arg; *p; p++) {
        if (*p == '"') {
            (*buf)[(*len)++] = '\\';
        }
        (*buf)[(*len)++] = *p;
    }
    if (quote)
        (*buf)[(*len)++] = '"';
    (*buf)[*len] = '\0';
}

static char *plat_win_commandline(const plat_run_t *r) {
    size_t len = 0, cap = 256;
    char *buf = malloc(cap);

    if (!buf)
        return NULL;
    buf[0] = '\0';
    if (r->cmd) {
        size_t need = strlen(r->cmd) + 32;

        free(buf);
        cap = need;
        buf = malloc(cap);
        if (!buf)
            return NULL;
        snprintf(buf, cap, "cmd.exe /d /s /c \"%s\"", r->cmd);
        return buf;
    }
    for (char *const *a = r->argv; *a; a++)
        plat_win_append_arg(&buf, &len, &cap, *a);
    return buf;
}

int plat_run(const plat_run_t *r) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE out_r = NULL, out_w = NULL, in_r = NULL, in_w = NULL, nul = NULL, job = NULL;
    PROCESS_INFORMATION pi;
    STARTUPINFOA si;
    char *cmdline = NULL;
    char chunk[4096];
    double deadline = r->timeout_s > 0 ? plat_mono() + r->timeout_s : 0;
    bool exited = false, killed = false;
    DWORD code = 0;
    int rc = -1;

    memset(&pi, 0, sizeof pi);
    memset(&si, 0, sizeof si);
    if (!CreatePipe(&out_r, &out_w, &sa, 0))
        return -1;
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    if (r->in) {
        if (!CreatePipe(&in_r, &in_w, &sa, 0)) {
            CloseHandle(out_r);
            CloseHandle(out_w);
            return -1;
        }
        SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    } else {
        nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                          OPEN_EXISTING, 0, NULL);
    }
    cmdline = plat_win_commandline(r);
    job = CreateJobObjectA(NULL, NULL);

    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = r->in ? in_r : nul;
    si.hStdOutput = out_w;
    si.hStdError = out_w;

    if (!cmdline ||
        !CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                        NULL, NULL, &si, &pi)) {
        if (r->missing_note && r->sink)
            r->sink(r->ctx, PLAT_MISSING_TEXT, sizeof PLAT_MISSING_TEXT - 1);
        rc = PLAT_EXIT_NOT_FOUND;
        goto done;
    }
    if (job)
        AssignProcessToJobObject(job, pi.hProcess); /* best effort: tree kill */
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(out_w);
    out_w = NULL;
    if (in_r) {
        DWORD off = 0;

        CloseHandle(in_r);
        in_r = NULL;
        while (off < r->inlen) {
            DWORD w = 0;

            if (!WriteFile(in_w, r->in + off, (DWORD)(r->inlen - off), &w, NULL) || w == 0)
                break;
            off += w;
        }
        CloseHandle(in_w);
        in_w = NULL;
    }

    for (;;) {
        DWORD avail = 0;

        if (!exited && WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0)
            exited = true;
        if (!killed && ((r->stop && *r->stop) || plat_win_timed_out(deadline))) {
            if (job)
                TerminateJobObject(job, 1);
            else
                TerminateProcess(pi.hProcess, 1);
            killed = true;
        }
        if (!PeekNamedPipe(out_r, NULL, 0, NULL, &avail, NULL))
            break; /* every writer has closed the pipe */
        if (avail > 0) {
            DWORD got = 0;

            if (ReadFile(out_r, chunk, avail < sizeof chunk ? avail : sizeof chunk, &got, NULL) &&
                got > 0 && r->sink)
                r->sink(r->ctx, chunk, got);
            continue;
        }
        if (exited)
            break;
        plat_sleep_ms(20);
    }

    if (!exited) {
        if (job)
            TerminateJobObject(job, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
    }
    if (job)
        TerminateJobObject(job, 1); /* leftover children from a finished command */
    if (killed) {
        rc = PLAT_EXIT_TIMEOUT;
    } else if (GetExitCodeProcess(pi.hProcess, &code)) {
        rc = (int)(code & 0xFF);
    }

done:
    free(cmdline);
    if (pi.hProcess)
        CloseHandle(pi.hProcess);
    if (job)
        CloseHandle(job);
    if (out_r)
        CloseHandle(out_r);
    if (out_w)
        CloseHandle(out_w);
    if (in_r)
        CloseHandle(in_r);
    if (in_w)
        CloseHandle(in_w);
    if (nul)
        CloseHandle(nul);
    return rc;
}

double plat_mono(void) {
    return (double)GetTickCount64() / 1000.0;
}

void plat_sleep_ms(int ms) {
    Sleep((DWORD)(ms < 0 ? 0 : ms));
}

bool plat_localtime(time_t t, struct tm *out) {
    struct tm *p = localtime(&t);

    if (!p)
        return false;
    *out = *p;
    return true;
}

bool plat_hostname(char *buf, size_t n) {
    DWORD len = (DWORD)n;

    if (n == 0 || !GetComputerNameA(buf, &len))
        return false;
    return true;
}

long plat_pid(void) {
    return (long)GetCurrentProcessId();
}

void plat_catch_stop(void (*handler)(int)) {
    signal(SIGINT, handler);
    signal(SIGTERM, handler);
#ifdef SIGBREAK
    signal(SIGBREAK, handler);
#endif
}

#else /* POSIX */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void plat_posix_nap(double secs) {
    struct timespec nap;

    nap.tv_sec = (time_t)secs;
    nap.tv_nsec = (long)((secs - (double)nap.tv_sec) * 1e9);
    nanosleep(&nap, NULL);
}

int plat_run(const plat_run_t *r) {
    int out_p[2];
    int in_p[2] = {-1, -1};
    pid_t pid;
    int status = 0;
    bool exited = false, eof = false, killed = false;
    double deadline = r->timeout_s > 0 ? plat_mono() + r->timeout_s : 0;
    char chunk[4096];

    if (pipe(out_p) < 0)
        return -1;
    if (r->in && pipe(in_p) < 0) {
        close(out_p[0]);
        close(out_p[1]);
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        close(out_p[0]);
        close(out_p[1]);
        if (r->in) {
            close(in_p[0]);
            close(in_p[1]);
        }
        return -1;
    }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDONLY);

        if (r->in) {
            dup2(in_p[0], STDIN_FILENO);
            close(in_p[0]);
            close(in_p[1]);
        } else if (nul >= 0) {
            dup2(nul, STDIN_FILENO);
        }
        if (nul >= 0)
            close(nul);
        dup2(out_p[1], STDOUT_FILENO);
        dup2(out_p[1], STDERR_FILENO);
        close(out_p[0]);
        close(out_p[1]);
        setpgid(0, 0); /* own process group, so a timeout kills the whole tree */
        if (r->cmd)
            execl("/bin/sh", "sh", "-c", r->cmd, (char *)NULL);
        else
            execvp(r->argv[0], r->argv);
        if (r->missing_note) {
            ssize_t w = write(STDOUT_FILENO, PLAT_MISSING_TEXT, sizeof PLAT_MISSING_TEXT - 1);
            (void)w; /* best effort: the child exits either way */
        }
        _exit(PLAT_EXIT_NOT_FOUND);
    }
    setpgid(pid, pid); /* also in the parent, so the group exists before any kill */
    close(out_p[1]);
    if (r->in) {
        size_t off = 0;

        close(in_p[0]);
        while (off < r->inlen) {
            ssize_t w = write(in_p[1], r->in + off, r->inlen - off);

            if (w < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            off += (size_t)w;
        }
        close(in_p[1]);
    }

    for (;;) {
        if (!exited && waitpid(pid, &status, WNOHANG) == pid)
            exited = true;
        if (!killed && ((r->stop && *r->stop) || (deadline > 0 && plat_mono() >= deadline))) {
            kill(-pid, SIGKILL);
            killed = true;
        }
        if (eof) {
            if (exited)
                break;
            plat_posix_nap(0.02); /* output closed; wait for the child to exit */
            continue;
        }
        {
            struct pollfd pf;
            double left = deadline > 0 ? deadline - plat_mono() : 0.2;
            int wait_ms = (int)(left * 1000.0);

            if (wait_ms > 200 || deadline == 0)
                wait_ms = 200;
            if (wait_ms < 1)
                wait_ms = 1;
            pf.fd = out_p[0];
            pf.events = POLLIN;
            pf.revents = 0;
            if (poll(&pf, 1, wait_ms) < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (pf.revents & (POLLIN | POLLHUP | POLLERR)) {
                ssize_t n = read(out_p[0], chunk, sizeof chunk);

                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0)
                    eof = true;
                else if (r->sink)
                    r->sink(r->ctx, chunk, (size_t)n);
            }
        }
    }
    close(out_p[0]);
    if (!exited) {
        if (!killed)
            kill(-pid, SIGKILL);
        killed = true;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
    }
    if (killed)
        return PLAT_EXIT_TIMEOUT;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

double plat_mono(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void plat_sleep_ms(int ms) {
    plat_posix_nap((ms < 0 ? 0 : ms) / 1000.0);
}

bool plat_localtime(time_t t, struct tm *out) {
    return localtime_r(&t, out) != NULL;
}

bool plat_hostname(char *buf, size_t n) {
    if (n == 0)
        return false;
    buf[n - 1] = '\0';
    return gethostname(buf, n - 1) == 0;
}

long plat_pid(void) {
    return (long)getpid();
}

void plat_catch_stop(void (*handler)(int)) {
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

#endif /* _WIN32 */


#if defined(_WIN32)
bool plat_replace_file(const char *from, const char *to) {
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) != 0;
}

bool plat_fsync(FILE *f) {
    return _commit(_fileno(f)) == 0;
}
#else
bool plat_replace_file(const char *from, const char *to) {
    return rename(from, to) == 0;
}

bool plat_fsync(FILE *f) {
    return fsync(fileno(f)) == 0;
}
#endif

#endif /* SRCT_PLATFORM_H */
