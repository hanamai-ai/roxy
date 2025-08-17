// epoll_server.c
// Minimal nonblocking epoll server that parses RESP3 frames from clients,
// using the span-based parser (handles pipelined requests).
//
// Build: gcc -Wall -O2 epoll_server.c redis_resp.c -o epoll_server
// Run:   ./epoll_server 6380
//
// Try:
//   printf '*1\r\n$4\r\nPING\r\n' | nc 127.0.0.1 6380
//   printf '*2\r\n$4\r\nECHO\r\n$5\r\nhello\r\n' | nc 127.0.0.1 6380
//   printf '*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n' | nc 127.0.0.1 6380
//   # pipelined in one go:
//   printf '*1\r\n$4\r\nPING\r\n*2\r\n$4\r\nECHO\r\n$3\r\nhey\r\n' | nc 127.0.0.1 6380

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#include "redis_resp.h"

#define MAX_EVENTS  128
#define READ_CHUNK  8192
#define BUF_LIMIT   (8*1024*1024) // 8MB per-conn cap

typedef struct {
    char  *data;
    size_t len;   // bytes valid
    size_t cap;
} Buf;

typedef struct {
    int fd;
    Buf in;
} Conn;

static void buf_init(Buf *b) { b->data=NULL; b->len=0; b->cap=0; }
static void buf_free(Buf *b) { free(b->data); b->data=NULL; b->len=b->cap=0; }

static int buf_reserve(Buf *b, size_t want) {
    if (want <= b->cap) return 1;
    size_t ncap = b->cap ? b->cap : 4096;
    while (ncap < want) ncap *= 2;
    char *nd = (char*)realloc(b->data, ncap);
    if (!nd) return 0;
    b->data = nd; b->cap = ncap;
    return 1;
}

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
    return 0;
}

static void close_conn(int ep, Conn *c) {
    if (!c) return;
    epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    buf_free(&c->in);
    free(c);
}

static void send_all_best_effort(int fd, const void *buf, size_t len) {
    const char *p = (const char*)buf;
    while (len) {
        ssize_t n = send(fd, p, len, 0);
        if (n > 0) { p += n; len -= (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        break; // give up on other errors to keep example short
    }
}

static void handle_command_and_reply(Conn *c, ParsedCommand *pc) {
    const char *reply = NULL;
    char tmp[512];

    const char **argv = NULL; size_t argc = 0;
    if (pc->root->type == RESP3_ARRAY) {
        (void)resp3_as_argv(pc->root, &argv, &argc);
    }

    switch (pc->cmd) {
        case CMD_PING:
            reply = "+PONG\r\n";
            break;
        case CMD_ECHO:
            if (argv && argc >= 2) {
                size_t mlen = strlen(argv[1]);
                int w = snprintf(tmp, sizeof(tmp), "$%zu\r\n", mlen);
                if (w > 0) send_all_best_effort(c->fd, tmp, (size_t)w);
                send_all_best_effort(c->fd, argv[1], mlen);
                send_all_best_effort(c->fd, "\r\n", 2);
                reply = NULL;
            } else {
                reply = "-ERR wrong number of arguments for 'ECHO'\r\n";
            }
            break;
        case CMD_SET:
            reply = "+OK\r\n";
            break;
        case CMD_GET:
            reply = "$-1\r\n"; // pretend nil
            break;
        default:
            reply = "-ERR unknown command\r\n";
            break;
    }

    if (reply) send_all_best_effort(c->fd, reply, strlen(reply));
    if (argv) free((void*)argv);
}

static void parse_loop_and_respond(int ep, Conn *c) {
    // Keep parsing frames as long as we can
    for (;;) {
        if (c->in.len == 0) return;

        size_t consumed = 0;
        ParsedCommand pc;
        Resp3ParseResult r = resp3_parse_command_span(c->in.data, c->in.len, &consumed, &pc);
        if (r == RESP3_PARSE_INCOMPLETE) {
            return; // need more bytes
        }
        if (r == RESP3_PARSE_ERROR) {
            // protocol error -> close
            fprintf(stderr, "Protocol error, closing fd=%d\n", c->fd);
            return close_conn(ep, c);
        }

        // Success
        handle_command_and_reply(c, &pc);
        resp3_free(pc.root);

        // Drop consumed bytes (compact buffer)
        if (consumed < c->in.len) {
            memmove(c->in.data, c->in.data + consumed, c->in.len - consumed);
        }
        c->in.len -= consumed;
    }
}

static void on_client_readable(int ep, Conn *c) {
    for (;;) {
        if (!buf_reserve(&c->in, c->in.len + READ_CHUNK)) {
            close_conn(ep, c); return;
        }
        ssize_t n = recv(c->fd, c->in.data + c->in.len, c->in.cap - c->in.len, 0);
        if (n > 0) {
            c->in.len += (size_t)n;
            if (c->in.len > BUF_LIMIT) { close_conn(ep, c); return; }
            continue; // try to read more this iteration (level-triggered)
        }
        if (n == 0) {
            // Peer closed; we may still have buffered data to parse
            parse_loop_and_respond(ep, c);
            return close_conn(ep, c);
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // No more data for now; parse what we have
            parse_loop_and_respond(ep, c);
            return;
        }
        if (errno == EINTR) continue;
        // Other recv error
        perror("recv");
        return close_conn(ep, c);
    }
}

static int run_server(uint16_t port) {
    signal(SIGPIPE, SIG_IGN);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("socket"); return 1; }
    int on=1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (set_nonblock(ls) < 0) { perror("nonblock"); close(ls); return 1; }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(ls); return 1; }
    if (listen(ls, 128) < 0) { perror("listen"); close(ls); return 1; }

    int ep = epoll_create1(0);
    if (ep < 0) { perror("epoll_create1"); close(ls); return 1; }

    struct epoll_event ev = {0};
    ev.events = EPOLLIN;
    ev.data.fd = ls;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, ls, &ev) < 0) { perror("epoll_ctl ADD ls"); close(ep); close(ls); return 1; }

    printf("Listening on 0.0.0.0:%u\n", (unsigned)port);

    struct epoll_event events[MAX_EVENTS];
    for (;;) {
        int n = epoll_wait(ep, events, MAX_EVENTS, -1);
        if (n < 0) { if (errno==EINTR) continue; perror("epoll_wait"); break; }
        for (int i=0;i<n;i++) {
            if (events[i].data.fd == ls) {
                // Accept as many as possible
                for (;;) {
                    struct sockaddr_in cli; socklen_t cl = sizeof(cli);
                    int cfd = accept(ls, (struct sockaddr*)&cli, &cl);
                    if (cfd < 0) {
                        if (errno==EAGAIN || errno==EWOULDBLOCK) break;
                        if (errno==EINTR) continue;
                        perror("accept"); break;
                    }
                    if (set_nonblock(cfd) < 0) { perror("nonblock(client)"); close(cfd); continue; }

                    Conn *c = (Conn*)calloc(1, sizeof(Conn));
                    if (!c) { close(cfd); continue; }
                    c->fd = cfd; buf_init(&c->in);

                    struct epoll_event cev = {0};
                    cev.events = EPOLLIN | EPOLLRDHUP;
                    cev.data.ptr = c;
                    if (epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &cev) < 0) {
                        perror("epoll_ctl ADD client");
                        close(cfd); free(c); continue;
                    }
                }
            } else {
                Conn *c = (Conn*)events[i].data.ptr;
                if (!c) continue;
                if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                    close_conn(ep, c);
                    continue;
                }
                if (events[i].events & EPOLLIN) {
                    on_client_readable(ep, c);
                }
            }
        }
    }

    close(ep);
    close(ls);
    return 0;
}

int main(int argc, char **argv) {
    uint16_t port = 6380;
    if (argc > 1) port = (uint16_t)atoi(argv[1]);
    return run_server(port);
}

