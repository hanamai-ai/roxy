#include "dbuf.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

void dbuf_init(struct dbuf *b) {
    b->data = nullptr;
    b->cap = 0;
    b->rpos = 0;
    b->wpos = 0;
}

void dbuf_free(struct dbuf *b) {
    free(b->data);
    b->data = nullptr;
    b->cap = 0;
    b->rpos = 0;
    b->wpos = 0;
}

size_t dbuf_len(const struct dbuf *b) {
    return b->wpos - b->rpos;
}

// C
static void dbuf_compact(struct dbuf *b) {
    if (b->rpos == 0)
        return;
    const size_t len = b->wpos - b->rpos;
    if (len)
        memmove(b->data, b->data + b->rpos, len);
    b->rpos = 0;
    b->wpos = len;
}

int dbuf_reserve(struct dbuf *b, size_t extra) {
    if (b->wpos + extra <= b->cap)
        return 0;
    if (b->rpos && (b->rpos + extra <= b->cap)) {
        dbuf_compact(b);
        return 0;
    }

    const size_t need = dbuf_len(b) + extra;
    size_t ncap = b->cap ? b->cap : 4096;
    while (ncap < need) {
        ncap *= 2;
    }
    char *nd = realloc(b->data, ncap);
    if (!nd)
        return -1;
    b->data = nd;
    b->cap = ncap;
    if (b->rpos)
        dbuf_compact(b);
    return 0;
}

int dbuf_append(struct dbuf *b, const void *data, const size_t n) {
    if (dbuf_reserve(b, n) < 0)
        return -1;
    memcpy(b->data + b->wpos, data, n);
    b->wpos += n;

    return 0;
}

void dbuf_consume(struct dbuf *b, const size_t n) {
    b->rpos += n;
    if (b->rpos > b->wpos)
        b->rpos = b->wpos;
    if (b->rpos == b->wpos) {
        b->rpos = 0 ;
        b->wpos = 0;
    }
}

ssize_t dbuf_read_from_fd(struct dbuf *b, int fd) {
    for (;;) {
        if (dbuf_reserve(b, 4096) < 0)
            return -1;
        const ssize_t n = read(fd, b->data + b->wpos, b->cap - b->wpos);
        if (n > 0) {
            b->wpos += (size_t)n;
            continue;
        }
        if (n == 0)
            return 0;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 1;
        if (errno == EINTR)
            continue;
        return -1;
    }
}

ssize_t dbuf_write_to_fd(struct dbuf *b, const int fd) {
    const size_t len = dbuf_len(b);
    if (!len)
        return 0;
    const ssize_t n = write(fd, b->data + b->rpos, len);
    if (n > 0) {
        dbuf_consume(b, (size_t)n);
        return n;
    }
    if (n < 0 && (errno==EAGAIN || errno==EWOULDBLOCK))
        return 0;
    if (n < 0 && errno==EINTR)
        return 0;
    
    return n;
}
