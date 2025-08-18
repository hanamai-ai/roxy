/*
 * roxy — common types & cleanup helpers
 * BSD-3-Clause
 *
 * This header centralizes lightweight utility types and RAII-like cleanup
 * helpers used across the project, along with public-facing server types.
 *
 * Overview:
 * - GNU-style cleanup attribute wrappers to simplify resource management.
 * - slice_t: non-owning (ptr,len) view over a byte/string range.
 * - roxy_cmd: parsed command representation (RESP-friendly).
 * - roxy_config: runtime configuration for the proxy/server.
 * - roxy_server_run: entry point to start the server loop.
 *
 * Notes:
 * - The cleanup helpers only trigger when variables go out of scope, similar
 *   to RAII; they assume C11-compatible compilers with GNU extensions.
 * - All slices are non-owning; the caller is responsible for backing storage
 *   lifetime unless documented otherwise by the specific API.
 */

#pragma once
#include <unistd.h>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>

/* Cleanup attribute wrapper
 *
 * Usage examples:
 *   _cleanup_free_ char *p = malloc(...);
 *   _cleanup_fclose_ FILE *fp = fopen(..., ...);
 *   _cleanup_close_ int fd = open(...);
 *
 * When the variable goes out of scope, the associated cleanup function is
 * invoked automatically if the value is valid/non-NULL.
 */
#define _cleanup_(f) __attribute__((cleanup(f)))

/* Invoked by _cleanup_free_: frees heap memory pointed to by the variable. */
static inline void roxy_cleanup_free(void *p)
{
	void **pp = p;
	if (pp && *pp)
		free(*pp);
}

/* Invoked by _cleanup_fclose_: fclose on non-NULL FILE*. */
static inline void roxy_cleanup_fclose(FILE **fp)
{
	if (fp && *fp)
		fclose(*fp);
}

/* Invoked by _cleanup_close_: close on valid (>=0) file descriptor. */
static inline void roxy_cleanup_close(int *fd)
{
	if (fd && *fd >= 0)
		close(*fd);
}

/* Shorthand macros for common cleanup use-cases. */
#define _cleanup_free_   _cleanup_(roxy_cleanup_free)
#define _cleanup_fclose_ _cleanup_(roxy_cleanup_fclose)
#define _cleanup_close_  _cleanup_(roxy_cleanup_close)

/* slice_t
 *
 * A non-owning view onto a contiguous memory region. Commonly used to pass
 * around string-like data without requiring null termination.
 *
 * Invariants:
 * - ptr may be NULL only if len == 0 (unless specified otherwise).
 * - The lifetime of ptr must outlive all consumers of the slice.
 */
typedef struct {
	const char *ptr;
	size_t len;
} slice_t;

/* Upper bound on the number of arguments in a parsed command, including
 * the command name itself. Can be overridden at compile time.
 */
#ifndef ROXY_MAX_ARGS
#define ROXY_MAX_ARGS 256
#endif

/* roxy_cmd
 *
 * Parsed command representation suitable for RESP-style processing.
 *
 * Fields:
 * - cmd: uppercased command name stored for quick comparisons (null-terminated
 *        if shorter than the fixed buffer; contents beyond the last char are
 *        unspecified).
 * - argc: total number of arguments including the command name.
 * - argv: array of slices for each argument in order; argv[0] mirrors the
 *         command name as received (not necessarily uppercased).
 *
 * Lifetime:
 * - Each slice in argv is non-owning and points into the source buffer, unless
 *   documented otherwise by the parser implementation. Ensure the backing
 *   buffer stays valid while roxy_cmd is in use.
 */
struct roxy_cmd {
	char cmd[64];		// uppercased command
	size_t argc;		// includes command itself
	slice_t argv[ROXY_MAX_ARGS];
};

/* roxy_config
 *
 * Runtime configuration for the proxy/server instance.
 *
 * Fields:
 * - roxy_host, roxy_port: bind address/port for the proxy itself.
 * - redis_host, redis_port: upstream Redis address/port.
 * - log_level: 0=debug, 1=info, 2=warn, 3=error.
 *
 * Notes:
 * - Strings are borrowed; they must remain valid for the duration of
 *   roxy_server_run.
 */
struct roxy_config {
	const char *roxy_host;
	unsigned short roxy_port;
	const char *redis_host;
	unsigned short redis_port;
	int log_level;		// 0=debug..3=error
};
