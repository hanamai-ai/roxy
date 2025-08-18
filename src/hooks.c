#include "hooks.h"
#include "log.h"
#include <string.h>

struct hook_entry { char name[64]; roxy_hook_fn fn; void *user; };

static struct hook_entry g_hooks[128];
static size_t g_hook_n = 0;

void roxy_register_hook(const char *cmd_upper, roxy_hook_fn fn, void *user) {
    if (g_hook_n >= sizeof(g_hooks)/sizeof(g_hooks[0])) return;
    size_t cpy = strlen(cmd_upper); if (cpy >= sizeof(g_hooks[0].name)) cpy = sizeof(g_hooks[0].name)-1;
    memcpy(g_hooks[g_hook_n].name, cmd_upper, cpy); g_hooks[g_hook_n].name[cpy]='\0';
    g_hooks[g_hook_n].fn = fn; g_hooks[g_hook_n].user = user;
    g_hook_n++;
}

enum roxy_hook_result roxy_hook_apply(const struct roxy_cmd *cmd, roxy_rewrite_t *out) {
    // 1) Wildcard hook (e.g., global logger) if present
    for (size_t i=0;i<g_hook_n;i++) {
        if (strcmp(g_hooks[i].name, "*")==0) {
            enum roxy_hook_result r = g_hooks[i].fn(cmd, out, g_hooks[i].user);
            if (r != ROXY_PASS) return r;
            break;
        }
    }
    // 2) Specific hook
    for (size_t i=0;i<g_hook_n;i++) {
        if (strcmp(g_hooks[i].name, cmd->cmd)==0) {
            return g_hooks[i].fn(cmd, out, g_hooks[i].user);
        }
    }
    return ROXY_PASS;
}

// ---- default hooks ----

static enum roxy_hook_result hook_log_all(const struct roxy_cmd *cmd, roxy_rewrite_t *out, void *user) {
    (void)out; (void)user;
    char preview[256];
    size_t off=0;
    for (size_t i=0; i<cmd->argc && i<6; i++) {
        size_t n = cmd->argv[i].len < 40 ? cmd->argv[i].len : 40;
        if (off + n + 1 >= sizeof(preview)) break;
        off += (size_t)snprintf(preview+off, sizeof(preview)-off, "%s%.*s", (i?" ":""), (int)n, cmd->argv[i].ptr);
    }
    LOGI("[HOOK:*] %s argc=%zu%s%.*s", cmd->cmd, cmd->argc, off?" preview=":"", (int)off, preview);
    return ROXY_PASS;
}

static enum roxy_hook_result hook_log_some(const struct roxy_cmd *cmd, roxy_rewrite_t *out, void *user) {
    (void)out; (void)user;
    char preview[256];
    size_t off=0;
    for (size_t i=0; i<cmd->argc && i<4; i++) {
        const size_t n = cmd->argv[i].len < 40 ? cmd->argv[i].len : 40;
        if (off + n + 1 >= sizeof(preview))
            break;
        off += (size_t)snprintf(preview+off, sizeof(preview)-off, "%s%.*s", (i?" ":""), (int)n, cmd->argv[i].ptr);
    }
    LOGI("CMD %s argc=%zu%s%.*s", cmd->cmd, cmd->argc, off?" preview=":"", (int)off, preview);
    return ROXY_PASS;
}

static enum roxy_hook_result hook_block_flush(const struct roxy_cmd *cmd, roxy_rewrite_t *out, void *user) {
    (void)out; (void)user;
    if (strcmp(cmd->cmd, "FLUSHALL")==0 || strcmp(cmd->cmd,"FLUSHDB")==0) return ROXY_BLOCK;
    return ROXY_PASS;
}

static enum roxy_hook_result hook_rewrite_keys(const struct roxy_cmd *cmd, roxy_rewrite_t *out, void *user) {
    (void)user;
    if (strcmp(cmd->cmd, "KEYS")==0 && cmd->argc==2) {
        out->argc = 6;
        static const char CMD[]="SCAN", ZERO[]="0", MATCH[]="MATCH", COUNT[]="COUNT", C1000[]="1000";
        out->argv[0]=(slice_t){CMD, sizeof(CMD)-1};
        out->argv[1]=(slice_t){ZERO,sizeof(ZERO)-1};
        out->argv[2]=(slice_t){MATCH,sizeof(MATCH)-1};
        out->argv[3]=cmd->argv[1];
        out->argv[4]=(slice_t){COUNT,sizeof(COUNT)-1};
        out->argv[5]=(slice_t){C1000,sizeof(C1000)-1};
        out->heap_owned=false;
        return ROXY_REWRITE;
    }
    return ROXY_PASS;
}

void roxy_hooks_init_defaults(void) {
    roxy_register_hook("*",       hook_log_all,  nullptr);
    roxy_register_hook("PING",     hook_log_some, nullptr);
    roxy_register_hook("FLUSHALL", hook_block_flush, nullptr);
    roxy_register_hook("FLUSHDB",  hook_block_flush, nullptr);
    roxy_register_hook("KEYS",     hook_rewrite_keys, nullptr);
}
