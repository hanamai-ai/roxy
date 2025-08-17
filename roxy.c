// roxy.c
// Simple, compact, production‑ready C23 epoll server for a line‑based protocol.
// Commands:
//   PING -> PONG\r\n
//   INFO -> key=value lines, ending with \r\n
//   QUIT -> BYE\r\n then close connection
//
// Build: cc -std=c2x -O2 -Wall -Wextra -Wpedantic -D_GNU_SOURCE roxy.c -o roxy
// Run: ./server [port]
// Test:    printf "PING\nINFO\nQUIT\n" | nc -N 127.0.0.1 6379

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include "redis_resp.h"

// ---- cleanup helpers (GCC/Clang __attribute__((cleanup))) -----------------
#ifndef __has_attribute
#  define __has_attribute(x) 0
#endif
#if __has_attribute(cleanup) || defined(__GNUC__)
#  define _cleanup_(x) __attribute__((cleanup(x)))
#else
#  define _cleanup_(x)
#endif

static void freep(void *p) {
    void **pp = (void**)p; if (pp && *pp) free(*pp);
}
static void closep(int *fd) {
    if (fd && *fd >= 0) close(*fd);
}
static void fclosep(FILE **fp) {
    if (fp && *fp) fclose(*fp);
}

// ---- logging ---------------------------------------------------------------
static void log_time_iso(char *out, size_t n) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm; gmtime_r(&ts.tv_sec, &tm);
    strftime(out, n, "%Y-%m-%dT%H:%M:%S", &tm);
}
static void vlogf(const char *level, const char *fmt, va_list ap) {
    char ts[32]; log_time_iso(ts, sizeof ts);
    fprintf(stderr, "%sZ [%s] ", ts, level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}
static void logif(const char *level, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vlogf(level, fmt, ap); va_end(ap);
}
#define LOGI(...) logif("INFO", __VA_ARGS__)
#define LOGW(...) logif("WARN", __VA_ARGS__)
#define LOGE(...) logif("ERROR", __VA_ARGS__)

// ---- utilities -------------------------------------------------------------
static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static int set_nonblock_cloexec(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    int eflags = fcntl(fd, F_GETFD, 0);
    if (eflags >= 0) (void)fcntl(fd, F_SETFD, eflags | FD_CLOEXEC);
    return 0;
}

static int xsend(int fd, const void *buf, size_t len) {
    const char *p = buf;
    size_t n = len;
    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w > 0) { p += w; n -= (size_t)w; continue; }
        if (w < 0 && (errno == EINTR)) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break; // don't block
        }
        return -1;
    }
    return 0;
}

static int xprintf(int fd, const char *fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    size_t to_write = (size_t)(n < (int)sizeof buf ? n : (int)sizeof buf);
    return xsend(fd, buf, to_write);
}

// ---- per-connection state --------------------------------------------------
typedef struct Conn {
    int fd;
    char *in;
    size_t in_len;
    size_t in_cap;
    char peer[80];
} Conn;

static Conn *conn_new(int fd) {
    Conn *c = calloc(1, sizeof *c);
    if (!c) return nullptr;
    c->fd = fd;
    c->in_cap = 4096;
    c->in = malloc(c->in_cap);
    if (!c->in) { free(c); return nullptr; }
    c->peer[0] = '\0';
    return c;
}

static void conn_free(Conn *c) {
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    free(c->in);
    free(c);
}

static void send_pong(Conn *c) {
    LOGI("PING from %s", c->peer[0] ? c->peer : "?");
    xsend(c->fd, "+PONG\r\n", 7);
}

static void send_info(Conn *c) {
    LOGI("INFO from %s", c->peer[0] ? c->peer : "?");
    struct utsname un = {0};
    uname(&un);

    struct sysinfo si = {0};
    sysinfo(&si);

    char host[256] = {0};
    gethostname(host, sizeof host - 1);

    time_t now = time(nullptr);
    struct tm tm; gmtime_r(&now, &tm);
    char iso[64]; strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", &tm);

    double load1 = 0, load5 = 0, load15 = 0;
#if defined(__linux__)
    if (si.loads[0] || si.loads[1] || si.loads[2]) {
        constexpr double s = 1.0 / 65536.0; // per sysinfo(2)
        load1 = si.loads[0]*s; load5 = si.loads[1]*s; load15 = si.loads[2]*s;
    }
#endif

    xprintf(c->fd,
        "time=%s\r\n"
        "hostname=%s\r\n"
        "sysname=%s\r\n"
        "release=%s\r\n"
        "version=%s\r\n"
        "machine=%s\r\n"
        "uptime_sec=%ld\r\n"
        "loadavg=%.2f,%.2f,%.2f\r\n"
        "mem_total_bytes=%llu\r\n"
        "mem_free_bytes=%llu\r\n"
        "procs=%u\r\n\r\n",
        iso,
        host,
        un.sysname, un.release, un.version, un.machine,
        (long)si.uptime,
        load1, load5, load15,
        (unsigned long long)si.totalram * si.mem_unit,
        (unsigned long long)si.freeram * si.mem_unit,
        si.procs);
}

static void handle_command(Conn *c, const char *line) {

    //size_t n = strlen(line);
    //if (n && line[n-1] == '\r') n--;
    //if (n == 0) return;

    //if (n == 4 && (strncmp(line, "PING", 4) == 0 || strncmp(line, "ping", 4) == 0)) { send_pong(c); return; }
    //if (n == 4 && strncmp(line, "INFO", 4) == 0) { send_info(c); return; }
    //if (n == 4 && strncmp(line, "QUIT", 4) == 0) {
    //    LOGI("QUIT from %s", c->peer[0] ? c->peer : "?");
    //    xsend(c->fd, "BYE\r\n", 6);
    //    c->fd = -1; // mark for close by caller
    //    return;
    //}
    printf("command: %s\n", line);

    //LOGW("unknown command from %s: '%.*s'", c->peer[0] ? c->peer : "?", (int)n, line);
    //ignore unknown commands for now
    //xprintf(c->fd, "ERR unknown_command\r\n");

    ParsedCommand pc;
    if (!resp3_parse_command(line, &pc)) {
        fprintf(stderr, "Parse error\n");
        return;
    }

    printf("Command enum = %d\n", pc.cmd);

    // If the top-level is an array of strings, get argv
    const char **argv;
    size_t argc;
    if (resp3_as_argv(pc.root, &argv, &argc)) {
        printf("argc=%zu\n", argc);
        for (size_t i = 0; i < argc; i++) {
            printf("argv[%zu] = %s\n", i, argv[i]);
        }
        free((void*)argv); // free the argv array (not the strings)
    }

    // Free the parsed structure
    resp3_free(pc.root);

}

static void process_readable(Conn *c) {
    for (;;) {
        if (c->in_len == c->in_cap) {
            size_t nc = c->in_cap * 2;
            char *np = realloc(c->in, nc);
            if (!np) { LOGE("realloc failed for %s", c->peer); return; }
            c->in = np; c->in_cap = nc;
        }
        const ssize_t r = recv(c->fd, c->in + c->in_len, c->in_cap - c->in_len, 0);
        if (r > 0) {
            c->in_len += (size_t)r;
            size_t start = 0;
            for (size_t i = 0; i < c->in_len; ++i) {
                if (c->in[i] == '\n') {
                    c->in[i] = '\0';
                    handle_command(c, c->in + start);
                    if (c->fd < 0) return; // QUIT or error
                    start = i + 1;
                }
            }
            if (start > 0) {
                memmove(c->in, c->in + start, c->in_len - start);
                c->in_len -= start;
            }
            continue;
        }
        if (r == 0) { LOGI("close %s", c->peer); c->fd = -1; return; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (errno == EINTR) continue;
        LOGE("recv error from %s: %s", c->peer, strerror(errno));
        c->fd = -1; return;
    }
}

// ---- server setup ----------------------------------------------------------
static int make_listener(uint16_t port) {
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_REUSEPORT
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
#endif

    struct sockaddr_in6 addr = {0};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any; // also accepts v4 via v6-mapped
    addr.sin6_port = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof addr) < 0) { close(fd); return -1; }
    if (listen(fd, SOMAXCONN) < 0) { close(fd); return -1; }
    if (set_nonblock_cloexec(fd) < 0) { close(fd); return -1; }
    return fd;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = { .sa_handler = on_signal }; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr); sigaction(SIGTERM, &sa, nullptr);

    uint16_t port = 6379;
    if (argv[1]) {
        const long p = strtol(argv[1], nullptr, 10);
        if (p > 0 && p < 65536) port = (uint16_t)p;
    }

    _cleanup_(closep) int listen_fd = make_listener(port);
    if (listen_fd < 0) { perror("listen"); return 1; }

    LOGI("listening on [::]:%u", port);

    _cleanup_(closep) int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0) { perror("epoll_create1"); return 1; }

    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = NULL };
    if (epoll_ctl(ep, EPOLL_CTL_ADD, listen_fd, &ev) < 0) { perror("epoll_ctl add listen"); return 1; }

    struct epoll_event events[128];

    while (!g_stop) {
        int n = epoll_wait(ep, events, (int)(sizeof events/sizeof events[0]), 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; ++i) {
            struct epoll_event *e = &events[i];
            if (e->data.ptr == NULL) {
                // new connection(s)
                for (;;) {
                    struct sockaddr_storage ss; socklen_t slen = sizeof ss;
                    int cfd = accept4(listen_fd, (struct sockaddr*)&ss, &slen, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        LOGE("accept4 error: %s", strerror(errno)); break;
                    }
                    Conn *c = conn_new(cfd);
                    if (!c) { close(cfd); LOGE("OOM on accept"); continue; }
                    // peer string
                    if (ss.ss_family == AF_INET) {
                        char ip[INET_ADDRSTRLEN]; struct sockaddr_in *sa = (struct sockaddr_in*)&ss;
                        inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof ip);
                        snprintf(c->peer, sizeof c->peer, "%s:%u", ip, ntohs(sa->sin_port));
                    } else if (ss.ss_family == AF_INET6) {
                        char ip[INET6_ADDRSTRLEN]; struct sockaddr_in6 *sa6 = (struct sockaddr_in6*)&ss;
                        inet_ntop(AF_INET6, &sa6->sin6_addr, ip, sizeof ip);
                        snprintf(c->peer, sizeof c->peer, "[%s]:%u", ip, ntohs(sa6->sin6_port));
                    } else {
                        snprintf(c->peer, sizeof c->peer, "unknown");
                    }
                    LOGI("accept %s fd=%d", c->peer, cfd);
                    struct epoll_event ce = { .events = EPOLLIN | EPOLLRDHUP, .data.ptr = c };
                    if (epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &ce) < 0) {
                        LOGE("epoll_ctl add conn failed: %s", strerror(errno));
                        conn_free(c);
                        continue;
                    }
                }
            } else {
                Conn *c = e->data.ptr;
                if (e->events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                    LOGI("hangup %s", c->peer);
                    conn_free(c);
                    continue;
                }
                if (e->events & EPOLLIN) {
                    process_readable(c);
                    if (c->fd < 0) {
                        conn_free(c);
                        continue;
                    }
                }
            }
        }
    }

    return 0;
}
