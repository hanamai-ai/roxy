// tests/dbuf_test.c - C
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include "dbuf.h"

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "ASSERT_TRUE failed: %s at %s:%d\n", #expr, __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

#define ASSERT_EQ_SIZE(a,b) do { \
    size_t _a = (a), _b = (b); \
    if (_a != _b) { \
        fprintf(stderr, "ASSERT_EQ_SIZE failed: %s (%zu) != %s (%zu) at %s:%d\n", \
                #a, _a, #b, _b, __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

#define ASSERT_EQ_INT(a,b) do { \
    int _a = (a), _b = (b); \
    if (_a != _b) { \
        fprintf(stderr, "ASSERT_EQ_INT failed: %s (%d) != %s (%d) at %s:%d\n", \
                #a, _a, #b, _b, __FILE__, __LINE__); \
        exit(1); \
    } \
} while (0)

static void set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	ASSERT_TRUE(flags >= 0);
	ASSERT_TRUE(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

static void test_init_free(void)
{
	struct dbuf b;
	dbuf_init(&b);
	ASSERT_EQ_SIZE(dbuf_len(&b), 0);
	ASSERT_TRUE(b.cap == 0);
	ASSERT_TRUE(b.data == nullptr);

	const char *msg = "abc";
	ASSERT_EQ_INT(dbuf_append(&b, msg, strlen(msg)), 0);
	ASSERT_EQ_SIZE(dbuf_len(&b), 3);

	dbuf_free(&b);
	ASSERT_EQ_SIZE(dbuf_len(&b), 0);
	ASSERT_TRUE(b.cap == 0);
	ASSERT_TRUE(b.data == nullptr);
}

static void test_append_and_consume(void)
{
	struct dbuf b;
	dbuf_init(&b);

	const char *msg = "hello";
	ASSERT_EQ_INT(dbuf_append(&b, msg, 5), 0);
	ASSERT_EQ_SIZE(dbuf_len(&b), 5);
	ASSERT_TRUE(memcmp(b.data + b.rpos, "hello", 5) == 0);

	dbuf_consume(&b, 2);
	ASSERT_EQ_SIZE(dbuf_len(&b), 3);
	ASSERT_TRUE(memcmp(b.data + b.rpos, "llo", 3) == 0);

	dbuf_consume(&b, 100);	// over-consume clamps and resets positions
	ASSERT_EQ_SIZE(dbuf_len(&b), 0);
	ASSERT_EQ_SIZE(b.rpos, 0);
	ASSERT_EQ_SIZE(b.wpos, 0);

	dbuf_free(&b);
}

static void test_compaction_via_reserve_and_append(void)
{
	struct dbuf b;
	dbuf_init(&b);

	const size_t first = 3000;
	char *buf = malloc(first);
	ASSERT_TRUE(buf != nullptr);
	for (size_t i = 0; i < first; ++i)
		buf[i] = (char)('A' + (i % 26));
	ASSERT_EQ_INT(dbuf_append(&b, buf, first), 0);

	const size_t consumed = 2000;
	dbuf_consume(&b, consumed);
	ASSERT_EQ_SIZE(dbuf_len(&b), first - consumed);
	size_t old_cap = b.cap;

	const size_t extra = 1500;
	char *extra_buf = malloc(extra);
	ASSERT_TRUE(extra_buf != nullptr);
	memset(extra_buf, 'z', extra);

	ASSERT_EQ_INT(dbuf_append(&b, extra_buf, extra), 0);
	ASSERT_EQ_SIZE(b.rpos, 0);
	ASSERT_TRUE(b.cap == old_cap);
	ASSERT_EQ_SIZE(dbuf_len(&b), (first - consumed) + extra);

	ASSERT_TRUE(memcmp(b.data, buf + consumed, first - consumed) == 0);
	ASSERT_TRUE(memcmp(b.data + (first - consumed), extra_buf, extra) == 0);

	free(buf);
	free(extra_buf);
	dbuf_free(&b);
}

static void test_read_from_fd_nonblocking(void)
{
	int fds[2];
	ASSERT_TRUE(pipe(fds) == 0);
	int rd = fds[0], wr = fds[1];

	set_nonblocking(rd);

	const char *payload = "abcdef";
	ASSERT_TRUE(write(wr, payload, 6) == 6);

	struct dbuf b;
	dbuf_init(&b);

	ssize_t r = dbuf_read_from_fd(&b, rd);
	ASSERT_EQ_INT(r, 1);
	ASSERT_EQ_SIZE(dbuf_len(&b), 6);
	ASSERT_TRUE(memcmp(b.data + b.rpos, payload, 6) == 0);

	close(wr);
	r = dbuf_read_from_fd(&b, rd);
	ASSERT_EQ_INT(r, 0);

	dbuf_free(&b);
	close(rd);
}

static void drain_pipe_some(int rd, size_t want)
{
	char tmp[4096];
	size_t left = want;
	while (left > 0) {
		ssize_t n =
		    read(rd, tmp, left > sizeof(tmp) ? sizeof(tmp) : left);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			perror("read");
			exit(1);
		}
		if (n == 0)
			break;
		left -= (size_t)n;
	}
}

static void test_write_to_fd_nonblocking(void)
{
	int fds[2];
	ASSERT_TRUE(pipe(fds) == 0);
	int rd = fds[0], wr = fds[1];

	int flags = fcntl(wr, F_GETFL, 0);
	ASSERT_TRUE(flags >= 0);
	ASSERT_TRUE(fcntl(wr, F_SETFL, flags | O_NONBLOCK) == 0);

	struct dbuf b;
	dbuf_init(&b);

	const size_t big = 1 << 20;	// 1 MiB
	char *buf = malloc(big);
	ASSERT_TRUE(buf != nullptr);
	memset(buf, 'Y', big);

	ASSERT_EQ_INT(dbuf_append(&b, buf, big), 0);
	ASSERT_TRUE(dbuf_len(&b) == big);

	ssize_t n1 = dbuf_write_to_fd(&b, wr);
	ASSERT_TRUE(n1 > 0);
	ASSERT_EQ_SIZE(dbuf_len(&b), big - (size_t)n1);

	ssize_t n2 = dbuf_write_to_fd(&b, wr);
	ASSERT_TRUE(n2 == 0 || n2 > 0);

	drain_pipe_some(rd, 16384);
	ssize_t n3 = dbuf_write_to_fd(&b, wr);
	ASSERT_TRUE(n3 >= 0);

	dbuf_consume(&b, dbuf_len(&b));
	ASSERT_EQ_INT(dbuf_write_to_fd(&b, wr), 0);

	free(buf);
	dbuf_free(&b);
	close(rd);
	close(wr);
}

int main(void)
{
	test_init_free();
	test_append_and_consume();
	test_compaction_via_reserve_and_append();
	test_read_from_fd_nonblocking();
	test_write_to_fd_nonblocking();
	printf("All dbuf tests passed.\n");
	return 0;
}
