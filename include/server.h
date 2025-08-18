#pragma once
#include "roxy.h"

/* roxy_server_run
 *
 * Start the server with the provided configuration and block until shutdown.
 *
 * Parameters:
 * - cfg: non-NULL pointer to an initialized configuration struct.
 *
 * Return value:
 * - 0 on clean shutdown; non-zero on error (implementation-defined codes).
 *
 * Thread-safety:
 * - This call is typically single-threaded at the entry point; internal
 *   concurrency, if any, is managed by the server implementation.
 */
int roxy_server_run(const struct roxy_config *cfg);
