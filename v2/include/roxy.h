/*
 * roxy — common types & cleanup helpers
 * BSD-3-Clause
 */
#pragma once
#include <unistd.h>
#ifndef _GNU_SOURCE
  #define _GNU_SOURCE
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

// GNU cleanup attribute wrappers
#define _cleanup_(f) __attribute__((cleanup(f)))

static inline void roxy_cleanup_free(void *p) {
    void **pp = p; if (pp && *pp) free(*pp);
}
static inline void roxy_cleanup_fclose(FILE **fp) {
    if (fp && *fp) fclose(*fp);
}
static inline void roxy_cleanup_close(int *fd) {
    if (fd && *fd >= 0) close(*fd);
}

#define _cleanup_free_   _cleanup_(roxy_cleanup_free)
#define _cleanup_fclose_ _cleanup_(roxy_cleanup_fclose)
#define _cleanup_close_  _cleanup_(roxy_cleanup_close)

typedef struct {
    const char *ptr;
    size_t len;
} slice_t;

#ifndef ROXY_MAX_ARGS
#define ROXY_MAX_ARGS 256
#endif

struct roxy_cmd {
    char   cmd[64];     // uppercased command
    size_t argc;        // includes command itself
    slice_t argv[ROXY_MAX_ARGS];
};

// Proxy config
struct roxy_config {
    const char *roxy_host;
    unsigned short roxy_port;
    const char *redis_host;
    unsigned short redis_port;
    int log_level; // 0=debug..3=error
};

// Server API
int roxy_server_run(const struct roxy_config *cfg);
