#pragma once
#include <stddef.h>
#include <sys/types.h>

struct dbuf {
    char  *data;
    size_t cap;
    size_t rpos;
    size_t wpos;
};

void   dbuf_init(struct dbuf *b);
void   dbuf_free(struct dbuf *b);
size_t dbuf_len(const struct dbuf *b);
int    dbuf_reserve(struct dbuf *b, size_t extra);
int    dbuf_append(struct dbuf *b, const void *data, size_t n);
void   dbuf_consume(struct dbuf *b, size_t n);
ssize_t dbuf_read_from_fd(struct dbuf *b, int fd);
ssize_t dbuf_write_to_fd(struct dbuf *b, int fd);
