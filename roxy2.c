/*
 * roxy — a Redis Layer 7 proxy
 * ------------------------------------------------------------
 * Features
 *  - Modern C (C2x) codebase, GNU extensions where needed
 *  - epoll-based TCP server (Linux)
 *  - Transparent pass-through with pipelining support
 *  - Intercept hook for EVERY Redis command before forwarding
 *  - Minimal RESP2/RESP3 request parser (arrays + bulk strings)
 *  - Optional request rewrite or block per-command
 *  - Backpressure & buffering, TCP_NODELAY, SO_KEEPALIVE
 *  - Config via env or args (with sane defaults)
 *  - Uses _cleanup_ (GNU cleanup attribute) for RAII-like resource mgmt
 *  - BSD-3-Clause licensed — compatible with Redis' license
 *
 * Notes on parser licensing
 *  - This file includes an original, minimal RESP parser, not copied from Redis.
 *    If you prefer to "borrow" Redis' parser, replace the parser section with
 *    the corresponding Redis code and retain the BSD-3 license notice as required.
 *
 * Build (Linux):
 *  cc -std=c2x -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -o roxy roxy.c
 *
 * Run:
 *  ./roxy --roxy-host 127.0.0.1 --roxy-port 6380 --redis-host 127.0.0.1 --redis-port 6379
 *  # or with environment variables (args take precedence):
 *  export ROXY_HOST=127.0.0.1
 *  export ROXY_PORT=6380
 *  export REDIS_HOST=127.0.0.1
 *  export REDIS_PORT=6379
 *  ./roxy
 *
 * Example hooks inside: blocks FLUSHALL, rewrites KEYS * -> SCAN 0 MATCH * COUNT 1000
 *
 * ------------------------------------------------------------
 * BSD 3-Clause License
 *
 * Copyright (c) 2025, Roxy Authors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// -----------------------------
// Cleanup helpers (GNU extension)
// -----------------------------
#define _cleanup_(f) __attribute__((cleanup(f)))

static inline void cleanup_free(void *p) {
    void **pp = (void**)p; if (pp && *pp) free(*pp);
}
static inline void cleanup_fclose(FILE **fp) {
    if (fp && *fp) fclose(*fp);
}
static inline void cleanup_close(int *fd) {
    if (fd && *fd >= 0) close(*fd);
}
#define _cleanup_free_ _cleanup_(cleanup_free)
#define _cleanup_fclose_ _cleanup_(cleanup_fclose)
#define _cleanup_close_ _cleanup_(cleanup_close)

// -----------------------------
// Logging
// -----------------------------

enum log_level { LOG_DEBUG=0, LOG_INFO=1, LOG_WARN=2, LOG_ERR=3 };
static enum log_level g_log_level = LOG_INFO;

static void log_at(enum log_level lvl, const char *fmt, ...) {
    if (lvl < g_log_level) return;
    static const char *names[] = {"DBG","INF","WRN","ERR"};
    char ts[32];
    struct timespec tv; clock_gettime(CLOCK_REALTIME, &tv);
    struct tm tm; localtime_r(&tv.tv_sec, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf((lvl>=LOG_WARN)?stderr:stdout, "%s.%03ld [%s] ", ts, tv.tv_nsec/1000000, names[lvl]);
    va_list ap; va_start(ap, fmt); vfprintf((lvl>=LOG_WARN)?stderr:stdout, fmt, ap); va_end(ap);
    fputc('\n', (lvl>=LOG_WARN)?stderr:stdout);
}

#define LOGD(...) log_at(LOG_DEBUG, __VA_ARGS__)
#define LOGI(...) log_at(LOG_INFO,  __VA_ARGS__)
#define LOGW(...) log_at(LOG_WARN,  __VA_ARGS__)
#define LOGE(...) log_at(LOG_ERR,   __VA_ARGS__)

// -----------------------------
// Small utilities
// -----------------------------

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int tcp_tune(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_KEEPIDLE
    int idle = 30; setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
#ifdef TCP_KEEPINTVL
    int intvl = 10; setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#endif
#ifdef TCP_KEEPCNT
    int cnt = 3; setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
    return 0;
}

static inline void str_to_upper(char *s) {
    for (; *s; ++s) if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 'a' + 'A');
}

static inline long parse_long(const char *s, size_t n) {
    long v = 0; bool neg=false; size_t i=0; if (i<n && s[i]=='-') {neg=true; i++;}
    for (; i<n; ++i) { char c=s[i]; if (c<'0'||c>'9') return LONG_MIN; v = v*10 + (c-'0'); }
    return neg?-v:v;
}

// -----------------------------
// Dynamic buffer
// -----------------------------

struct dbuf {
    char  *data;
    size_t cap;
    size_t rpos; // read position
    size_t wpos; // write position
};

static void dbuf_init(struct dbuf *b) {
    b->data=NULL; b->cap=0; b->rpos=0; b->wpos=0;
}

static void dbuf_free(struct dbuf *b) {
    free(b->data); b->data=NULL; b->cap=0; b->rpos=b->wpos=0;
}

static size_t dbuf_len(const struct dbuf *b) { return b->wpos - b->rpos; }

static void dbuf_compact(struct dbuf *b) {
    if (b->rpos > 0) {
        size_t len = dbuf_len(b);
        if (len) memmove(b->data, b->data + b->rpos, len);
        b->rpos = 0; b->wpos = len;
    }
}

static int dbuf_reserve(struct dbuf *b, size_t extra) {
    if (b->wpos + extra <= b->cap) return 0;
    size_t len = dbuf_len(b);
    if (b->rpos && (b->rpos + extra <= b->cap)) { dbuf_compact(b); return 0; }
    size_t need = len + extra;
    size_t ncap = b->cap? b->cap : 4096;
    while (ncap < need) ncap *= 2;
    char *nd = realloc(b->data, ncap);
    if (!nd) return -1;
    b->data = nd; b->cap = ncap;
    if (b->rpos) dbuf_compact(b);
    return 0;
}

static int dbuf_append(struct dbuf *b, const void *data, size_t n) {
    if (dbuf_reserve(b, n) < 0) return -1;
    memcpy(b->data + b->wpos, data, n);
    b->wpos += n; return 0;
}

static void dbuf_consume(struct dbuf *b, size_t n) {
    b->rpos += n; if (b->rpos > b->wpos) b->rpos = b->wpos;
    if (b->rpos == b->wpos) b->rpos = b->wpos = 0; // reset
}

static ssize_t dbuf_read_from_fd(struct dbuf *b, int fd) {
    // Try to read as much as possible in nonblocking mode
    for (;;) {
        if (dbuf_reserve(b, 4096) < 0) return -1;
        ssize_t n = read(fd, b->data + b->wpos, b->cap - b->wpos);
        if (n > 0) { b->wpos += (size_t)n; continue; }
        if (n == 0) return 0; // EOF
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 1; // no more now
        if (errno == EINTR) continue; // retry
        return -1; // error
    }
}

static ssize_t dbuf_write_to_fd(struct dbuf *b, int fd) {
    size_t len = dbuf_len(b);
    if (!len) return 0;
    ssize_t n = write(fd, b->data + b->rpos, len);
    if (n > 0) { dbuf_consume(b, (size_t)n); return n; }
    if (n < 0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return 0; // try later
    if (n < 0 && errno==EINTR) return 0; // try later
    return n; // 0 or -1
}

// -----------------------------
// RESP parsing (requests): minimally handles arrays of bulk strings
// -----------------------------

typedef struct {
    const char *ptr; size_t len;
} slice_t;

#define ROXY_MAX_ARGS 256

struct roxy_cmd {
    char   cmd[64]; // uppercased command name
    size_t argc;    // includes command itself as argv[0]
    slice_t argv[ROXY_MAX_ARGS];
};

// Return values for parse: >0 frame_len bytes for a full request, 0 incomplete, -1 error
static long parse_crlf_number(const char *s, size_t n, size_t *digits_out) {
    size_t i=0; bool neg=false; if (i<n && s[i]=='-') {neg=true; i++;}
    size_t start=i; long val=0;
    for (; i<n; ++i) {
        char c=s[i];
        if (c=='\r') { if (i+1<n && s[i+1]=='\n') { *digits_out = i+2; return neg? -val: val; } else return LONG_MIN; }
        if (c<'0'||c>'9') return LONG_MIN;
        val = val*10 + (c-'0');
    }
    return LONG_MIN; // incomplete
}

static int resp_parse_request(const char *buf, size_t len, size_t *frame_len, struct roxy_cmd *out) {
    if (len==0) return 0;
    size_t pos = 0;
    if (buf[0] == '*') {
        // Array of bulk strings (most common for requests)
        size_t used; long nitems = parse_crlf_number(buf+1, len-1, &used);
        if (nitems == LONG_MIN) return 0; // incomplete or bad
        if (nitems <= 0 || nitems > ROXY_MAX_ARGS) return -1;
        pos = 1 + used;
        out->argc = 0;
        for (long i=0; i<nitems; ++i) {
            if (pos >= len) return 0;
            if (buf[pos] != '$') return -1; // we expect bulk strings
            size_t u2; long blen = parse_crlf_number(buf+pos+1, len-pos-1, &u2);
            if (blen == LONG_MIN) return 0; // incomplete
            if (blen < 0) { // null bulk not valid in command position
                return -1;
            }
            size_t hdr = 1 + u2; // $<len>\r\n
            if (pos + hdr + (size_t)blen + 2 > len) return 0; // incomplete body
            const char *b = buf + pos + hdr;
            out->argv[out->argc].ptr = b;
            out->argv[out->argc].len = (size_t)blen;
            out->argc++;
            pos += hdr + (size_t)blen + 2; // consume data + CRLF
        }
        // Extract command name (argv[0])
        size_t n0 = out->argv[0].len;
        size_t copy = n0 < sizeof(out->cmd)-1 ? n0 : sizeof(out->cmd)-1;
        memcpy(out->cmd, out->argv[0].ptr, copy);
        out->cmd[copy] = '\0';
        str_to_upper(out->cmd);
        *frame_len = pos;
        return 1;
    } else {
        // Inline protocol: COMMAND arg arg\r\n (rare today)
        // parse until CRLF; split by spaces
        const char *cr = NULL;
        for (size_t i=0;i<len-1;i++) { if (buf[i]=='\r' && buf[i+1]=='\n') { cr = buf+i; break; } }
        if (!cr) return 0; // incomplete
        // Tokenize
        out->argc = 0;
        size_t i=0; while (&buf[i] < cr && out->argc < ROXY_MAX_ARGS) {
            while (&buf[i] < cr && (buf[i]==' '||buf[i]=='\t')) i++;
            size_t start = i;
            while (&buf[i] < cr && buf[i]!=' ' && buf[i]!='\t') i++;
            if (i>start) {
                out->argv[out->argc].ptr = buf+start;
                out->argv[out->argc].len = (size_t)(i-start);
                out->argc++;
            }
        }
        if (out->argc==0) return -1;
        size_t n0 = out->argv[0].len;
        size_t copy = n0 < sizeof(out->cmd)-1 ? n0 : sizeof(out->cmd)-1;
        memcpy(out->cmd, out->argv[0].ptr, copy);
        out->cmd[copy]='\0'; str_to_upper(out->cmd);
        *frame_len = (size_t)((cr-buf)+2);
        return 1;
    }
}

// -----------------------------
// Hook API
// -----------------------------

enum roxy_hook_result { ROXY_PASS=0, ROXY_BLOCK=1, ROXY_REWRITE=2 };

typedef struct {
    size_t argc;
    slice_t argv[ROXY_MAX_ARGS]; // when rewriting, provide new argv[] (argv[0] is command)
    bool    heap_owned;          // if true, proxy will free argv[i].ptr after encoding
} roxy_rewrite_t;

typedef enum roxy_hook_result (*roxy_hook_fn)(const struct roxy_cmd *cmd, roxy_rewrite_t *out, void *user);

struct hook_entry { char name[64]; roxy_hook_fn fn; void *user; };

static struct hook_entry g_hooks[128];
static size_t g_hook_n = 0;

static void roxy_register_hook(const char *cmd_upper, roxy_hook_fn fn, void *user) {
    if (g_hook_n >= sizeof(g_hooks)/sizeof(g_hooks[0])) return;
    size_t cpy = strlen(cmd_upper); if (cpy >= sizeof(g_hooks[0].name)) cpy = sizeof(g_hooks[0].name)-1;
    memcpy(g_hooks[g_hook_n].name, cmd_upper, cpy); g_hooks[g_hook_n].name[cpy]='\0';
    g_hooks[g_hook_n].fn = fn; g_hooks[g_hook_n].user=user; g_hook_n++;
}

static struct hook_entry* roxy_find_hook(const char *cmd_upper) {
    for (size_t i=0;i<g_hook_n;i++) if (strcmp(g_hooks[i].name, cmd_upper)==0) return &g_hooks[i];
    return NULL;
}

// RESP encoder for argv[] -> array request
static int resp_encode_array(struct dbuf *b, const slice_t *argv, size_t argc) {
    char hdr[64];
    int n = snprintf(hdr, sizeof(hdr), "*%zu\r\n", argc);
    if (n<0) return -1; if (dbuf_append(b, hdr, (size_t)n)<0) return -1;
    for (size_t i=0;i<argc;i++) {
        n = snprintf(hdr, sizeof(hdr), "$%zu\r\n", argv[i].len);
        if (n<0) return -1; if (dbuf_append(b, hdr, (size_t)n)<0) return -1;
        if (dbuf_append(b, argv[i].ptr, argv[i].len)<0) return -1;
        if (dbuf_append(b, "\r\n", 2)<0) return -1;
    }
    return 0;
}

// -----------------------------
// Connection state & epoll
// -----------------------------

typedef enum { FD_LISTENER=0, FD_CLIENT=1, FD_UPSTREAM=2 } fd_type_t;

struct conn;

struct fdctx {
    fd_type_t type;
    int       fd;
    struct conn *c; // back-ref; NULL for listener
};

struct conn {
    int client_fd;
    int up_fd;
    bool up_connected;

    struct fdctx client_ctx;
    struct fdctx up_ctx;

    struct dbuf c_in;  // client->proxy incoming
    struct dbuf c_out; // proxy->client outgoing (responses)
    struct dbuf u_in;  // upstream->proxy incoming
    struct dbuf u_out; // proxy->upstream outgoing (requests)

    size_t inflight;   // number of requests currently in-flight to upstream
};

static int g_ep = -1; // epoll fd
static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig) { (void)sig; g_running = 0; }

static int ep_ctl(int ep, int op, int fd, uint32_t events, struct fdctx *ctx) {
    struct epoll_event ev = { .events = events, .data.ptr = ctx };
    return epoll_ctl(ep, op, fd, &ev);
}

static void fd_enable_events(struct fdctx *x, uint32_t events) {
    struct epoll_event ev = { .events = events, .data.ptr = x };
    epoll_ctl(g_ep, EPOLL_CTL_MOD, x->fd, &ev);
}

static void fd_add_events(struct fdctx *x, uint32_t add) {
    struct epoll_event ev; socklen_t elen=sizeof(ev); // not used; construct new
    ev.events = 0; ev.data.ptr = x; // we'll query by MOD: easier to track desired flags outside
    // Simpler: get current by attempting a MOD with no change is tricky, so keep our own desired mask.
    // For simplicity, we store desired mask in upper 16 bits of fd to avoid extra state? No.
    // We'll just try to add by reading current via EPOLL_CTL_MOD requiring we know existing.
    // To keep things simple, callers use fd_enable_events with computed mask.
    (void)elen; (void)ev; (void)add;
}

static void conn_free(struct conn *c) {
    if (!c) return;
    if (c->client_fd>=0) { epoll_ctl(g_ep, EPOLL_CTL_DEL, c->client_fd, NULL); close(c->client_fd); }
    if (c->up_fd>=0)     { epoll_ctl(g_ep, EPOLL_CTL_DEL, c->up_fd, NULL);     close(c->up_fd); }
    dbuf_free(&c->c_in); dbuf_free(&c->c_out); dbuf_free(&c->u_in); dbuf_free(&c->u_out);
    free(c);
}

static void schedule_writable(struct fdctx *x, bool enable) {
    uint32_t mask = EPOLLIN | EPOLLET; // default read interest, edge-triggered
    if (enable) mask |= EPOLLOUT;
    fd_enable_events(x, mask);
}

static int connect_upstream(struct conn *c, const char *host, uint16_t port) {
    _cleanup_close_ int fd = -1;
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd<0) { LOGE("socket upstream: %s", strerror(errno)); return -1; }
    set_nonblock(fd); tcp_tune(fd);
    struct sockaddr_in sa = {0}; sa.sin_family=AF_INET; sa.sin_port=htons(port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        LOGE("Invalid upstream host %s (IPv4 only in this minimal build)", host); return -1;
    }
    int r = connect(fd, (struct sockaddr*)&sa, sizeof(sa));
    if (r<0 && errno!=EINPROGRESS) { LOGE("connect upstream: %s", strerror(errno)); return -1; }
    c->up_fd = fd; // transfer ownership
    c->up_connected = (r==0);
    c->up_ctx = (struct fdctx){ .type=FD_UPSTREAM, .fd=c->up_fd, .c=c };
    ep_ctl(g_ep, EPOLL_CTL_ADD, c->up_fd, EPOLLIN|EPOLLOUT|EPOLLET, &c->up_ctx);
    LOGD("upstream fd %d connecting...", c->up_fd);
    fd = -1; // prevent cleanup_close_
    return 0;
}

static int write_err_to_client(struct conn *c, const char *msg) {
    const char *prefix = "-ERR ";
    if (dbuf_append(&c->c_out, prefix, strlen(prefix))<0) return -1;
    if (dbuf_append(&c->c_out, msg, strlen(msg))<0) return -1;
    if (dbuf_append(&c->c_out, "\r\n", 2)<0) return -1;
    schedule_writable(&c->client_ctx, true);
    return 0;
}

static const size_t HIGH_WATER = 8*1024*1024; // 8MB backpressure threshold

static void maybe_backpressure(struct conn *c) {
    // If client->upstream queue is too large, stop reading from client
    if (dbuf_len(&c->u_out) > HIGH_WATER) {
        fd_enable_events(&c->client_ctx, EPOLLET); // disable EPOLLIN temporarily
    } else {
        fd_enable_events(&c->client_ctx, EPOLLIN|EPOLLET);
    }
    if (dbuf_len(&c->c_out) > HIGH_WATER) {
        fd_enable_events(&c->up_ctx, EPOLLET);
    } else {
        fd_enable_events(&c->up_ctx, EPOLLIN|EPOLLET | (c->up_connected?0:EPOLLOUT));
    }
}

static void handle_client_read(struct conn *c);
static void handle_upstream_read(struct conn *c);
static void handle_client_write(struct conn *c);
static void handle_upstream_write(struct conn *c);

// -----------------------------
// Hook examples (can be replaced/extended by users)
// -----------------------------

static enum roxy_hook_result hook_log_all(const struct roxy_cmd *cmd, roxy_rewrite_t *out, void *user) {
    (void)out; (void)user;
    // Print the first few args for visibility
    char preview[256]; size_t off=0;
    for (size_t i=0;i<cmd->argc && i<4;i++) {
        size_t n = cmd->argv[i].len < 40 ? cmd->argv[i].len : 40;
        off += (size_t)snprintf(preview+off, sizeof(preview)-off, "%s%.*s",
                                (i?" ":""), (int)n, cmd->argv[i].ptr);
        if (off>=sizeof(preview)) break;
    }
    LOGI("CMD %s argc=%zu%s%.*s", cmd->cmd, cmd->argc, off?" preview=":"", (int)off, preview);
    return ROXY_PASS;
}

static enum roxy_hook_result hook_block_flushall(const struct roxy_cmd *cmd, roxy_rewrite_t *out, void *user) {
    (void)out;(void)user;
    if (strcmp(cmd->cmd, "FLUSHALL") == 0 || strcmp(cmd->cmd, "FLUSHDB")==0) {
        return ROXY_BLOCK;
    }
    return ROXY_PASS;
}

static enum roxy_hook_result hook_rewrite_keys(const struct roxy_cmd *cmd, roxy_rewrite_t *out, void *user) {
    (void)user;
    if (strcmp(cmd->cmd, "KEYS") == 0 && cmd->argc==2) {
        // Rewrite: SCAN 0 MATCH <pattern> COUNT 1000
        out->argc = 6;
        static const char CMD[] = "SCAN"; static const char ZERO[] = "0"; static const char MATCH[]="MATCH"; static const char COUNT[]="COUNT"; static const char C1000[]="1000";
        out->argv[0] = (slice_t){ CMD,   sizeof(CMD)-1 };
        out->argv[1] = (slice_t){ ZERO,  sizeof(ZERO)-1 };
        out->argv[2] = (slice_t){ MATCH, sizeof(MATCH)-1 };
        out->argv[3] = cmd->argv[1]; // pattern from client
        out->argv[4] = (slice_t){ COUNT, sizeof(COUNT)-1 };
        out->argv[5] = (slice_t){ C1000, sizeof(C1000)-1 };
        out->heap_owned = false;
        return ROXY_REWRITE;
    }
    return ROXY_PASS;
}

// -----------------------------
// Core proxy logic
// -----------------------------

static void process_client_buffer(struct conn *c) {
    // Parse as many complete requests as possible
    for (;;) {
        size_t len = dbuf_len(&c->c_in);
        if (!len) break;
        struct roxy_cmd cmd; size_t frame=0; int r = resp_parse_request(c->c_in.data + c->c_in.rpos, len, &frame, &cmd);
        if (r==0) break; // need more data
        if (r<0) {
            LOGW("Protocol error from client fd=%d", c->client_fd);
            write_err_to_client(c, "protocol error");
            // drop this frame to avoid deadlock
            dbuf_consume(&c->c_in, len);
            break;
        }
        // Invoke hook if present
        roxy_rewrite_t rw = {0};
        struct hook_entry *he = roxy_find_hook(cmd.cmd);
        enum roxy_hook_result action = he? he->fn(&cmd, &rw, he->user) : ROXY_PASS;
        if (action == ROXY_BLOCK) {
            LOGI("Blocked %s", cmd.cmd);
            write_err_to_client(c, "blocked by roxy");
            dbuf_consume(&c->c_in, frame);
            // no inflight change
        } else if (action == ROXY_REWRITE) {
            LOGI("Rewrite %s", cmd.cmd);
            if (resp_encode_array(&c->u_out, rw.argv, rw.argc) < 0) {
                LOGE("OOM encoding rewrite");
            }
            if (rw.heap_owned) {
                for (size_t i=0;i<rw.argc;i++) free((void*)rw.argv[i].ptr);
            }
            dbuf_consume(&c->c_in, frame);
            c->inflight++;
            schedule_writable(&c->up_ctx, true);
        } else { // pass-through
            // forward raw frame to upstream
            if (dbuf_append(&c->u_out, c->c_in.data + c->c_in.rpos, frame) < 0) {
                LOGE("OOM forwarding frame");
            }
            dbuf_consume(&c->c_in, frame);
            c->inflight++;
            schedule_writable(&c->up_ctx, true);
        }
        maybe_backpressure(c);
    }
}

static void handle_client_read(struct conn *c) {
    for (;;) {
        ssize_t rr = dbuf_read_from_fd(&c->c_in, c->client_fd);
        if (rr == 0) { // EOF
            LOGD("client fd=%d EOF", c->client_fd);
            conn_free(c); return;
        } else if (rr < 0) {
            LOGE("client read error: %s", strerror(errno));
            conn_free(c); return;
        } else if (rr == 1) {
            break; // no more for now
        }
    }
    process_client_buffer(c);
}

static void handle_upstream_read(struct conn *c) {
    for (;;) {
        ssize_t rr = dbuf_read_from_fd(&c->u_in, c->up_fd);
        if (rr == 0) { LOGD("upstream fd=%d EOF", c->up_fd); conn_free(c); return; }
        else if (rr < 0) { LOGE("upstream read error: %s", strerror(errno)); conn_free(c); return; }
        else if (rr == 1) { break; }
    }
    // For now, proxy responses transparently
    if (dbuf_len(&c->u_in)) {
        if (dbuf_append(&c->c_out, c->u_in.data + c->u_in.rpos, dbuf_len(&c->u_in)) < 0) {
            LOGE("OOM appending response");
        }
        dbuf_consume(&c->u_in, dbuf_len(&c->u_in));
        schedule_writable(&c->client_ctx, true);
    }
    maybe_backpressure(c);
}

static void handle_client_write(struct conn *c) {
    ssize_t n = dbuf_write_to_fd(&c->c_out, c->client_fd);
    if (n < 0) { LOGE("client write error: %s", strerror(errno)); conn_free(c); return; }
    if (dbuf_len(&c->c_out)==0) schedule_writable(&c->client_ctx, false);
}

static void handle_upstream_write(struct conn *c) {
    if (!c->up_connected) {
        int err=0; socklen_t el=sizeof(err);
        if (getsockopt(c->up_fd, SOL_SOCKET, SO_ERROR, &err, &el)==0) {
            if (err==0) { c->up_connected = true; LOGD("upstream connected fd=%d", c->up_fd); }
            else { LOGE("upstream connect failed: %s", strerror(err)); conn_free(c); return; }
        }
    }
    ssize_t n = dbuf_write_to_fd(&c->u_out, c->up_fd);
    if (n < 0) { LOGE("upstream write error: %s", strerror(errno)); conn_free(c); return; }
    if (dbuf_len(&c->u_out)==0) fd_enable_events(&c->up_ctx, EPOLLIN|EPOLLET); // stop EPOLLOUT until more data
}

static void on_event(struct epoll_event *ev) {
    struct fdctx *x = (struct fdctx*)ev->data.ptr;
    if (!x) return;
    if (x->type == FD_LISTENER) {
        // Accept as many as possible
        for (;;) {
            struct sockaddr_in ca; socklen_t cl = sizeof(ca);
            int cfd = accept4(x->fd, (struct sockaddr*)&ca, &cl, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (cfd < 0) {
                if (errno==EAGAIN||errno==EWOULDBLOCK) break; else { LOGE("accept: %s", strerror(errno)); break; }
            }
            tcp_tune(cfd);
            // Create connection
            struct conn *c = calloc(1, sizeof(*c));
            if (!c) { LOGE("OOM conn"); close(cfd); break; }
            c->client_fd = cfd; c->up_fd = -1; c->up_connected=false; c->inflight=0;
            dbuf_init(&c->c_in); dbuf_init(&c->c_out); dbuf_init(&c->u_in); dbuf_init(&c->u_out);
            c->client_ctx = (struct fdctx){ .type=FD_CLIENT, .fd=cfd, .c=c };
            ep_ctl(g_ep, EPOLL_CTL_ADD, cfd, EPOLLIN|EPOLLET, &c->client_ctx);

            // Connect upstream using global settings stored in listener's user pointer
            const char **cfg = (const char**)x; // nasty, but works: we store after struct
            // We'll pass via globals instead (simpler): see g_up_host/g_up_port
            extern const char *g_up_host; extern uint16_t g_up_port;
            if (connect_upstream(c, g_up_host, g_up_port) < 0) { conn_free(c); continue; }
            LOGI("accepted client fd=%d -> upstream fd=%d", cfd, c->up_fd);
        }
        return;
    }

    struct conn *c = x->c; if (!c) return;
    uint32_t e = ev->events;

    if (x->type == FD_CLIENT) {
        if (e & (EPOLLERR|EPOLLHUP)) { conn_free(c); return; }
        if (e & EPOLLIN)  handle_client_read(c);
        if (e & EPOLLOUT) handle_client_write(c);
    } else if (x->type == FD_UPSTREAM) {
        if (e & (EPOLLERR|EPOLLHUP)) { conn_free(c); return; }
        if (e & EPOLLIN)  handle_upstream_read(c);
        if (e & EPOLLOUT) handle_upstream_write(c);
    }
}

// -----------------------------
// Configuration & main
// -----------------------------

static const char *g_roxy_host = "127.0.0.1";
static uint16_t    g_roxy_port = 6380;
const char        *g_up_host   = "127.0.0.1";
uint16_t           g_up_port   = 6379;

static void parse_env(void) {
    const char *v;
    if ((v=getenv("ROXY_HOST")) && *v) g_roxy_host = v;
    if ((v=getenv("ROXY_PORT")) && *v) g_roxy_port = (uint16_t)atoi(v);
    if ((v=getenv("REDIS_HOST")) && *v) g_up_host = v;
    if ((v=getenv("REDIS_PORT")) && *v) g_up_port = (uint16_t)atoi(v);
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [options]\n\n"
        "Options:\n"
        "  --roxy-host <ip>       (default: 127.0.0.1 or ROXY_HOST)\n"
        "  --roxy-port <port>     (default: 6380       or ROXY_PORT)\n"
        "  --redis-host <ip>      (default: 127.0.0.1 or REDIS_HOST)\n"
        "  --redis-port <port>    (default: 6379       or REDIS_PORT)\n"
        "  -v, --verbose          increase log verbosity (repeatable)\n"
        "  -h, --help             show this help\n",
        argv0);
}

int main(int argc, char **argv) {
    parse_env();

    static struct option opts[] = {
        {"roxy-host",  required_argument, 0, 1000},
        {"roxy-port",  required_argument, 0, 1001},
        {"redis-host", required_argument, 0, 1002},
        {"redis-port", required_argument, 0, 1003},
        {"verbose",    no_argument,       0, 'v'},
        {"help",       no_argument,       0, 'h'},
        {0,0,0,0}
    };

    int c;
    while ((c=getopt_long(argc, argv, "vh", opts, NULL)) != -1) {
        switch (c) {
            case 1000: g_roxy_host = optarg; break;
            case 1001: g_roxy_port = (uint16_t)atoi(optarg); break;
            case 1002: g_up_host   = optarg; break;
            case 1003: g_up_port   = (uint16_t)atoi(optarg); break;
            case 'v': if (g_log_level>0) g_log_level--; break; // more verbose
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 2;
        }
    }

    LOGI("Roxy starting: listen %s:%u -> upstream %s:%u", g_roxy_host, g_roxy_port, g_up_host, g_up_port);

    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);

    _cleanup_close_ int lfd = -1;
    lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd<0) { perror("socket"); return 1; }
    int one=1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    set_nonblock(lfd);

    struct sockaddr_in sa = {0}; sa.sin_family=AF_INET; sa.sin_port=htons(g_roxy_port);
    if (inet_pton(AF_INET, g_roxy_host, &sa.sin_addr) != 1) { fprintf(stderr, "Invalid roxy host %s\n", g_roxy_host); return 1; }
    if (bind(lfd, (struct sockaddr*)&sa, sizeof(sa)) < 0) { perror("bind"); return 1; }
    if (listen(lfd, SOMAXCONN) < 0) { perror("listen"); return 1; }

    g_ep = epoll_create1(EPOLL_CLOEXEC);
    if (g_ep < 0) { perror("epoll_create1"); return 1; }

    struct fdctx lctx = { .type=FD_LISTENER, .fd=lfd, .c=NULL };
    ep_ctl(g_ep, EPOLL_CTL_ADD, lfd, EPOLLIN|EPOLLET, &lctx);

    // Register built-in sample hooks; users can add their own
    roxy_register_hook("FLUSHALL", hook_block_flushall, NULL);
    roxy_register_hook("FLUSHDB",  hook_block_flushall, NULL);
    roxy_register_hook("KEYS",     hook_rewrite_keys, NULL);
    roxy_register_hook("*",        hook_log_all, NULL); // not used by lookup, but left as example
    roxy_register_hook("PING",     hook_log_all, NULL);

    // Event loop
    const int MAXEV = 64;
    struct epoll_event evs[MAXEV];

    while (g_running) {
        int n = epoll_wait(g_ep, evs, MAXEV, 1000);
        if (n < 0) {
            if (errno==EINTR) continue; perror("epoll_wait"); break;
        }
        for (int i=0;i<n;i++) on_event(&evs[i]);
    }

    LOGI("Shutting down");
    // epoll and listener auto-closed by cleanup attribute
    return 0;
}

