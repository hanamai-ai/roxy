
/*
 * Roxy - lightweight TCP proxy for Redis with command hooks and RESP parsing.
 *
 * This file implements an edge-triggered epoll-based proxy that:
 * - Accepts client connections.
 * - Optionally rewrites or blocks client requests via hooks.
 * - Forwards requests upstream to Redis and relays responses back.
 * - Applies simple backpressure using high-water marks to avoid unbounded memory growth.
 * - Uses non-blocking sockets and ET epoll for scalability.
 *
 * All I/O is performed through small helpers in dbuf.* to handle partial reads/writes.
 * The protocol handling for requests is performed via resp.* utilities.
 */

#include "server.h"
#include "resp.h"
#include "dbuf.h"
#include "hooks.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* Type of file descriptor registered in epoll */
typedef enum { FD_LISTENER = 0, FD_CLIENT = 1, FD_UPSTREAM = 2 } fd_type_t;

/* Forward declaration */
struct conn;

/* Small context stored in epoll's data.ptr for each FD */
struct fdctx {
	fd_type_t type;		/* what this fd is */
	int fd;			/* the actual file descriptor */
	struct conn *c;		/* owning connection (NULL for listener) */
};

/* Per-connection state storing both client and upstream */
struct conn {
	int client_fd;		/* client socket */
	int up_fd;		/* upstream (Redis) socket */
	bool up_connected;	/* finished non-blocking connect() */

	struct fdctx client_ctx;	/* epoll ctx for client fd */
	struct fdctx up_ctx;	/* epoll ctx for upstream fd */

	/* I/O buffers:
	 *  - c_in: bytes read from client not yet processed/parsing
	 *  - c_out: bytes to write to client (responses/errors)
	 *  - u_in: bytes read from upstream, not yet forwarded to client
	 *  - u_out: bytes to write to upstream (requests)
	 */
	struct dbuf c_in;
	struct dbuf c_out;
	struct dbuf u_in;
	struct dbuf u_out;

	/* Number of in-flight client requests forwarded upstream (best-effort) */
	size_t inflight;
};

/* Global epoll fd and run flag */
static int g_ep = -1;
static volatile sig_atomic_t g_running = 1;
/* Global config (set on server start) */
static const struct roxy_config *g_cfg = nullptr;

/* Stop main loop on SIGINT/SIGTERM */
static void on_signal(const int sig)
{
	(void)sig;
	g_running = 0;
}

/* Set O_NONBLOCK on fd */
static int set_nonblock(int fd)
{
	const int fl = fcntl(fd, F_GETFL, 0);
	if (fl < 0)
		return -1;
	return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Best-effort TCP socket tuning for proxying latency-sensitive traffic */
static int tcp_tune(const int fd)
{
	const int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_KEEPIDLE
	const int idle = 30;
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
#ifdef TCP_KEEPINTVL
	int intvl = 10;
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#endif
#ifdef TCP_KEEPCNT
	int cnt = 3;
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
	return 0;
}

/* Helper for epoll_ctl with fdctx payload */
static int ep_ctl(const int ep, int op, const int fd, const uint32_t events,
		  struct fdctx *ctx)
{
	struct epoll_event ev = {.events = events,.data.ptr = ctx };
	return epoll_ctl(ep, op, fd, &ev);
}

/* Modify interest set for an already-registered fd */
static void fd_enable_events(struct fdctx *x, const uint32_t events)
{
	struct epoll_event ev = {.events = events,.data.ptr = x };
	epoll_ctl(g_ep, EPOLL_CTL_MOD, x->fd, &ev);
}

/* Close both sides of a connection and free resources.
 * Safe to call multiple times; checks for negative fds.
 */
static void conn_free(struct conn *c)
{
	if (!c)
		return;
	if (c->client_fd >= 0) {
		epoll_ctl(g_ep, EPOLL_CTL_DEL, c->client_fd, nullptr);
		close(c->client_fd);
	}
	if (c->up_fd >= 0) {
		epoll_ctl(g_ep, EPOLL_CTL_DEL, c->up_fd, nullptr);
		close(c->up_fd);
	}
	dbuf_free(&c->c_in);
	dbuf_free(&c->c_out);
	dbuf_free(&c->u_in);
	dbuf_free(&c->u_out);
	free(c);
}

/* Enable or disable EPOLLOUT while keeping EPOLLIN for ET.
 * Used to wake the peer writer only when we have bytes queued.
 */
static void schedule_writable(struct fdctx *x, bool enable)
{
	uint32_t mask = EPOLLIN | EPOLLET;
	if (enable)
		mask |= EPOLLOUT;
	fd_enable_events(x, mask);
}

/* Send a RESP-simple-error-like line to the client, then make client fd writable */
static int write_err_to_client(struct conn *c, const char *msg)
{
	const char *prefix = "-ERR ";
	if (dbuf_append(&c->c_out, prefix, strlen(prefix)) < 0)
		return -1;
	if (dbuf_append(&c->c_out, msg, strlen(msg)) < 0)
		return -1;
	if (dbuf_append(&c->c_out, "\r\n", 2) < 0)
		return -1;
	schedule_writable(&c->client_ctx, true);
	return 0;
}

/* Initiate a non-blocking connect() to the configured upstream Redis */
static int connect_upstream(struct conn *c)
{
	const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		LOGE("socket upstream: %s", strerror(errno));
		return -1;
	}
	set_nonblock(fd);
	tcp_tune(fd);
	struct sockaddr_in sa = { 0 };
	sa.sin_family = AF_INET;
	sa.sin_port = htons(g_cfg->redis_port);
	if (inet_pton(AF_INET, g_cfg->redis_host, &sa.sin_addr) != 1) {
		LOGE("Invalid upstream host %s (IPv4 only in this build)",
		     g_cfg->redis_host);
		close(fd);
		return -1;
	}
	int r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
	if (r < 0 && errno != EINPROGRESS) {
		LOGE("connect upstream: %s", strerror(errno));
		close(fd);
		return -1;
	}
	c->up_fd = fd;
	c->up_connected = (r == 0);
	c->up_ctx = (struct fdctx) {.type = FD_UPSTREAM,.fd = c->up_fd,.c = c };
	/* Register upstream with EPOLLIN | EPOLLOUT initially:
	 * - EPOLLOUT drives completion of non-blocking connect.
	 * - EPOLLIN allows reading as soon as connection is established.
	 */
	ep_ctl(g_ep, EPOLL_CTL_ADD, c->up_fd, EPOLLIN | EPOLLOUT | EPOLLET,
	       &c->up_ctx);
	LOGD("upstream fd %d connecting...", c->up_fd);
	return 0;
}

/* Backpressure threshold per direction. Prevents unbounded buffering. */
static const size_t HIGH_WATER = 8 * 1024 * 1024;

/* Apply backpressure by muting reads on one side when the opposite write buffer grows.
 * Because we use edge-triggered epoll, we must preserve EPOLLOUT when we have pending
 * bytes to write; otherwise we could stall.
 */
static void maybe_backpressure(struct conn *c)
{
	// Client side: throttle reads based on upstream-out buffer size, but
	// preserve EPOLLOUT if we have data to write to the client.
	uint32_t client_mask = EPOLLET;
	if (dbuf_len(&c->u_out) <= HIGH_WATER)
		client_mask |= EPOLLIN;
	if (dbuf_len(&c->c_out) > 0)
		client_mask |= EPOLLOUT;
	fd_enable_events(&c->client_ctx, client_mask);

	// Upstream side: throttle reads based on client-out buffer size, but
	// preserve EPOLLOUT if we have data to write upstream or we’re still connecting.
	uint32_t up_mask = EPOLLET;
	if (dbuf_len(&c->c_out) <= HIGH_WATER)
		up_mask |= EPOLLIN;
	if (!c->up_connected || dbuf_len(&c->u_out) > 0)
		up_mask |= EPOLLOUT;
	fd_enable_events(&c->up_ctx, up_mask);
}

/* Parse and process any complete requests in c->c_in:
 * - On protocol error: send -ERR and drop the remaining bytes.
 * - On hook BLOCK: reply -ERR and drop the frame (do not forward).
 * - On hook REWRITE: encode new argv into u_out and mark inflight.
 * - On PASS-THROUGH: forward raw frame bytes into u_out and mark inflight.
 * After enqueuing to u_out, ensure upstream EPOLLOUT is enabled.
 */
static void process_client_buffer(struct conn *c)
{
	for (;;) {
		const size_t len = dbuf_len(&c->c_in);
		if (!len)
			break;	/* no more data to parse */

		/* Attempt to parse one RESP request frame without consuming input;
		 * frame returns the byte length of the complete request if available.
		 */
		struct roxy_cmd cmd;
		size_t frame = 0;
		int r =
		    resp_parse_request(c->c_in.data + c->c_in.rpos, len, &frame,
				       &cmd);
		if (r == 0)
			break;	/* incomplete - wait for more bytes */
		if (r < 0) {
			/* Protocol error: inform client and drain input to resync */
			LOGW("Protocol error from client fd=%d", c->client_fd);
			write_err_to_client(c, "protocol error");
			dbuf_consume(&c->c_in, len);
			break;
		}

		/* Let hooks decide whether to block, rewrite, or pass-through */
		roxy_rewrite_t rw = { 0 };
		enum roxy_hook_result action = roxy_hook_apply(&cmd, &rw);
		if (action == ROXY_BLOCK) {
			LOGI("Blocked %s", cmd.cmd);
			write_err_to_client(c, "blocked by roxy");
			dbuf_consume(&c->c_in, frame);
		} else if (action == ROXY_REWRITE) {
			LOGI("Rewrite %s", cmd.cmd);
			if (resp_encode_array(&c->u_out, rw.argv, rw.argc) < 0) {
				LOGE("OOM encoding rewrite");
			}
			/* Free any heap-allocated rewrite argv per hook contract */
			if (rw.heap_owned) {
				for (size_t i = 0; i < rw.argc; i++)
					free((void *)rw.argv[i].ptr);
			}
			dbuf_consume(&c->c_in, frame);
			c->inflight++;
			schedule_writable(&c->up_ctx, true);	/* wake upstream writer */
		} else {
			/* Pass-through: forward raw frame to upstream */
			if (dbuf_append
			    (&c->u_out, c->c_in.data + c->c_in.rpos,
			     frame) < 0) {
				LOGE("OOM forwarding frame");
			}
			dbuf_consume(&c->c_in, frame);
			c->inflight++;
			schedule_writable(&c->up_ctx, true);
			LOGD("Forwarded %s", cmd.cmd);
		}
		maybe_backpressure(c);
	}
}

/* Drain as much as possible from client into c_in; then try to parse/forward. */
static void handle_client_read(struct conn *c)
{
	for (;;) {
		const ssize_t rr = dbuf_read_from_fd(&c->c_in, c->client_fd);
		if (rr == 0) {
			/* Peer closed */
			LOGD("client fd=%d EOF", c->client_fd);
			conn_free(c);
			return;
		}
		if (rr < 0) {
			LOGE("client read error: %s", strerror(errno));
			conn_free(c);
			return;
		}
		if (rr == 1) {
			/* EAGAIN for non-blocking read - no more bytes right now */
			break;
		}
	}
	process_client_buffer(c);
}

/* Drain as much as possible from upstream into u_in; then move to client c_out. */
static void handle_upstream_read(struct conn *c)
{
	for (;;) {
		const ssize_t rr = dbuf_read_from_fd(&c->u_in, c->up_fd);
		if (rr == 0) {
			/* Upstream closed; tear down the connection */
			LOGW("Upstream Redis %s:%u disconnected (fd=%d EOF)",
			     g_cfg->redis_host, g_cfg->redis_port, c->up_fd);
			conn_free(c);
			return;
		}
		if (rr < 0) {
			LOGE("upstream read error: %s", strerror(errno));
			conn_free(c);
			return;
		}
		if (rr == 1) {
			/* EAGAIN - done reading for now */
			LOGD("upstream fd=%d EOF", c->up_fd);
			break;
		}
	}
	if (dbuf_len(&c->u_in)) {
		/* Move all available upstream response bytes to client-out buffer */
		LOGD("upstream fd=%d -> client fd=%d", c->up_fd, c->client_fd);
		if (dbuf_append
		    (&c->c_out, c->u_in.data + c->u_in.rpos,
		     dbuf_len(&c->u_in)) < 0) {
			LOGE("OOM appending response");
		} else {
			LOGD("Added %zu bytes: %.*s", dbuf_len(&c->u_in),
			     (int)dbuf_len(&c->u_in), c->u_in.data);
		}
		dbuf_consume(&c->u_in, dbuf_len(&c->u_in));
		/* We have something to write to client, but do not force EPOLLOUT if already set */
		schedule_writable(&c->client_ctx, false);
	}
	maybe_backpressure(c);
}

/* Attempt to write pending bytes to client; if drained, disable EPOLLOUT */
static void handle_client_write(struct conn *c)
{
	const ssize_t n = dbuf_write_to_fd(&c->c_out, c->client_fd);
	if (n < 0) {
		LOGE("client write error: %s", strerror(errno));
		conn_free(c);
		return;
	}
	if (dbuf_len(&c->c_out) == 0)
		schedule_writable(&c->client_ctx, false);
}

/* Complete non-blocking connect if needed, then flush queued requests to upstream */
static void handle_upstream_write(struct conn *c)
{
	if (!c->up_connected) {
		/* Check connect() completion via SO_ERROR */
		int err = 0;
		socklen_t el = sizeof(err);
		if (getsockopt(c->up_fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0) {
			if (err == 0) {
				c->up_connected = true;
				LOGD("upstream connected fd=%d", c->up_fd);
			} else {
				LOGE("upstream connect failed: %s",
				     strerror(err));
				conn_free(c);
				return;
			}
		}
	}
	LOGD("writing %zu bytes to upstream fd=%d", dbuf_len(&c->u_out),
	     c->up_fd);
	const ssize_t n = dbuf_write_to_fd(&c->u_out, c->up_fd);
	if (n < 0) {
		LOGE("upstream write error: %s", strerror(errno));
		conn_free(c);
		return;
	}
	/* If we fully drained u_out, we can drop EPOLLOUT to avoid busy loops */
	if (dbuf_len(&c->u_out) == 0)
		fd_enable_events(&c->up_ctx, EPOLLIN | EPOLLET);
}

/* Single event dispatcher called from the main loop.
 * - For listener: accept many clients in a loop (ET) and set up connection state.
 * - For client/upstream: route to respective read/write handlers.
 */
static void on_event(struct epoll_event *ev)
{
	struct fdctx *x = ev->data.ptr;
	if (!x)
		return;
	if (x->type == FD_LISTENER) {
		/* Edge-triggered accept loop until EAGAIN */
		for (;;) {
			struct sockaddr_in ca;
			socklen_t cl = sizeof(ca);
			const int cfd =
			    accept4(x->fd, (struct sockaddr *)&ca, &cl,
				    SOCK_NONBLOCK | SOCK_CLOEXEC);
			if (cfd < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					break;
				LOGE("accept: %s", strerror(errno));
				break;
			}
			setsockopt(cfd, SOL_SOCKET, SO_REUSEADDR, &(int) { 1 },
				   sizeof(int));
			tcp_tune(cfd);

			/* Allocate per-connection object */
			struct conn *c = calloc(1, sizeof(*c));
			if (!c) {
				LOGE("OOM conn");
				close(cfd);
				break;
			}

			/* Initialize conn state */
			c->client_fd = cfd;
			c->up_fd = -1;
			c->up_connected = false;
			c->inflight = 0;

			dbuf_init(&c->c_in);
			dbuf_init(&c->c_out);
			dbuf_init(&c->u_in);
			dbuf_init(&c->u_out);

			/* Register client fd */
			c->client_ctx = (struct fdctx) {.type = FD_CLIENT,.fd =
				    cfd,.c = c };
			ep_ctl(g_ep, EPOLL_CTL_ADD, cfd, EPOLLIN | EPOLLET,
			       &c->client_ctx);

			/* Initiate upstream connect; if it fails, tear down connection */
			if (connect_upstream(c) < 0) {
				conn_free(c);
				continue;
			}

			/* Log new connection */
			char ip[64];
			inet_ntop(AF_INET, &ca.sin_addr, ip, sizeof(ip));
			LOGI("accepted %s:%u fd=%d -> upstream fd=%d", ip,
			     ntohs(ca.sin_port), cfd, c->up_fd);
		}
		return;
	}

	/* Non-listener: must have a connection owner */
	struct conn *c = x->c;
	if (!c)
		return;
	const uint32_t e = ev->events;

	if (x->type == FD_CLIENT) {
		/* Close on error/hup; otherwise service read/write readiness */
		if (e & (EPOLLERR | EPOLLHUP)) {
			conn_free(c);
			return;
		}
		if (e & EPOLLIN)
			handle_client_read(c);
		if (e & EPOLLOUT)
			handle_client_write(c);
	} else if (x->type == FD_UPSTREAM) {
		if (e & (EPOLLERR | EPOLLHUP)) {
			LOGW("Upstream Redis %s:%u socket error/hang-up (fd=%d)", g_cfg->redis_host, g_cfg->redis_port, x->fd);
			conn_free(c);
			return;
		}
		if (e & EPOLLIN)
			handle_upstream_read(c);
		if (e & EPOLLOUT)
			handle_upstream_write(c);
	}
}

/* Entry point: set up listener, epoll, and event loop.
 * Returns 0 on clean shutdown, non-zero on early error.
 */
int roxy_server_run(const struct roxy_config *cfg)
{
	g_cfg = cfg;
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	/* Create listener */
	const int lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (lfd < 0) {
		perror("socket");
		return 1;
	}
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &(int) { 1 }, sizeof(int));
	set_nonblock(lfd);

	/* Bind to configured host:port */
	struct sockaddr_in sa = { 0 };
	sa.sin_family = AF_INET;
	sa.sin_port = htons(cfg->roxy_port);

	if (inet_pton(AF_INET, cfg->roxy_host, &sa.sin_addr) != 1) {
		fprintf(stderr, "Invalid roxy host %s\n", cfg->roxy_host);
		close(lfd);
		return 1;
	}
	if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("bind");
		close(lfd);
		return 1;
	}
	if (listen(lfd, SOMAXCONN) < 0) {
		perror("listen");
		close(lfd);
		return 1;
	}

	/* Create epoll instance and register listener (ET) */
	g_ep = epoll_create1(EPOLL_CLOEXEC);
	if (g_ep < 0) {
		perror("epoll_create1");
		close(lfd);
		return 1;
	}

	struct fdctx lctx = {.type = FD_LISTENER,.fd = lfd,.c = nullptr };
	ep_ctl(g_ep, EPOLL_CTL_ADD, lfd, EPOLLIN | EPOLLET, &lctx);

	const int MAXEV = 64;
	struct epoll_event evs[MAXEV];

	LOGI("Roxy listening on %s:%u → Redis %s:%u", cfg->roxy_host,
	     cfg->roxy_port, cfg->redis_host, cfg->redis_port);

	/* Main event loop: 1s epoll_wait timeout allows signal polling and graceful stop */
	while (g_running) {
		const int n = epoll_wait(g_ep, evs, MAXEV, 1000);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("epoll_wait");
			break;
		}
		for (int i = 0; i < n; i++)
			on_event(&evs[i]);
	}

	close(lfd);
	return 0;
}
