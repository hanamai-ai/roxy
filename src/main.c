/*
 * roxy — Redis L7 proxy (multi-file)
 * BSD-3-Clause
 */
#include "roxy.h"
#include "server.h"
#include "hooks.h"
#include "log.h"

#include <getopt.h>
#include <stdlib.h>

static void parse_env(struct roxy_config *cfg) {
    const char *v;
    if ((v=getenv("ROXY_HOST")) && *v) cfg->roxy_host = v;
    if ((v=getenv("ROXY_PORT")) && *v) cfg->roxy_port = (unsigned short)atoi(v);
    if ((v=getenv("REDIS_HOST")) && *v) cfg->redis_host = v;
    if ((v=getenv("REDIS_PORT")) && *v) cfg->redis_port = (unsigned short)atoi(v);
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [options]\n\n"
        "Options:\n"
        "  --roxy-host <ip>       (default: 0.0.0.0 or ROXY_HOST)\n"
        "  --roxy-port <port>     (default: 6380       or ROXY_PORT)\n"
        "  --redis-host <ip>      (default: 127.0.0.1 or REDIS_HOST)\n"
        "  --redis-port <port>    (default: 6379       or REDIS_PORT)\n"
        "  -v, --verbose          increase log verbosity (repeatable)\n"
        "  -h, --help             show this help\n",
        argv0);
}

int main(int argc, char **argv) {
    struct roxy_config cfg = {
        .roxy_host = "0.0.0.0",
        .roxy_port = 6380,
        .redis_host = "127.0.0.1",
        .redis_port = 6379,
        .log_level = LOG_INFO
    };

    parse_env(&cfg);

    static struct option opts[] = {
        {"roxy-host",  required_argument, 0, 1000},
        {"roxy-port",  required_argument, 0, 1001},
        {"redis-host", required_argument, 0, 1002},
        {"redis-port",  required_argument, 0, 1003},
        {"verbose",    no_argument,       0, 'v'},
        {"help",       no_argument,       0, 'h'},
        {0,0,0,0}
    };

    int c;
    while ((c=getopt_long(argc, argv, "vh", opts, nullptr)) != -1) {
        switch (c) {
            case 1000: cfg.roxy_host = optarg; break;
            case 1001: cfg.roxy_port = (unsigned short)atoi(optarg); break;
            case 1002: cfg.redis_host = optarg; break;
            case 1003: cfg.redis_port = (unsigned short)atoi(optarg); break;
            case 'v': if (cfg.log_level>0) cfg.log_level--; break;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 2;
        }
    }
    g_log_level = cfg.log_level;

    // Register default hooks (safe examples)
    roxy_hooks_init_defaults();

    return roxy_server_run(&cfg);
}
