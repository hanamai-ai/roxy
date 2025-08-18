#pragma once
#include "roxy.h"

enum roxy_hook_result { ROXY_PASS=0, ROXY_BLOCK=1, ROXY_REWRITE=2 };

typedef struct {
    size_t argc;
    slice_t argv[ROXY_MAX_ARGS];
    bool    heap_owned;
} roxy_rewrite_t;

typedef enum roxy_hook_result (*roxy_hook_fn)(const struct roxy_cmd *cmd, roxy_rewrite_t *out, void *user);

void roxy_register_hook(const char *cmd_upper, roxy_hook_fn fn, void *user);
enum roxy_hook_result roxy_hook_apply(const struct roxy_cmd *cmd, roxy_rewrite_t *out);

// Default example hooks (safe to remove/replace)
void roxy_hooks_init_defaults(void);
