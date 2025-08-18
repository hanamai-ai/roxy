// C RESP (Redis Serialization Protocol) helpers.
// This header declares minimal parsing/encoding routines for RESP requests,

#pragma once

#include "roxy.h"		// struct roxy_cmd definition (parsed command representation)
#include "dbuf.h"		// dynamic buffer utilities used for encoding
#include <stddef.h>

/*
 * Parse a single RESP request frame from a byte buffer.
 *
 * Parameters:
 *   buf       - Pointer to a contiguous memory region containing raw bytes.
 *   len       - Number of bytes available in 'buf'.
 *   frame_len - Out parameter; on success, set to the exact number of bytes
 *               that constitute the complete RESP frame at the start of 'buf'.
 *               This can be used by the caller to advance the input cursor.
 *   out       - Out parameter; on success, populated with the parsed command.
 *
 * Behavior:
 *   - The function attempts to parse exactly one RESP array frame representing
 *     a single request (e.g., ["SET","key","val"]).
 *   - If the buffer does not contain a complete frame, the function returns
 *     a status indicating "incomplete" and does not modify 'out' (unless
 *     otherwise documented by the implementation).
 *   - On malformed input, an error status is returned.
 *
 * Return value:
 *   Implementation-defined status code:
 *     - Success: parsing completed and 'out' is populated; '*frame_len' set.
 *     - Incomplete: more data is required to complete the frame.
 *     - Error: input is invalid; parsing failed.
 *
 * Notes:
 *   - Ownership/lifetime of any memory referenced by fields within 'out'
 *     (if any) is implementation-specific. Callers should consult the
 *     corresponding .c file to determine whether 'out' references 'buf'
 *     or copies data.
 *   - 'frame_len' is only meaningful on successful parse of a complete frame.
 */
int resp_parse_request(const char *buf, size_t len, size_t *frame_len,
		       struct roxy_cmd *out);

/*
 * Encode an array of bulk strings as a RESP array into a dynamic buffer.
 *
 * Parameters:
 *   b     - Destination dynamic buffer to append the encoded RESP frame into.
 *   argv  - Pointer to an array of slice_t elements; each element represents
 *           one bulk string (data pointer + length).
 *   argc  - Number of elements in 'argv'.
 *
 * Behavior:
 *   - Appends a RESP array header followed by argc bulk strings to 'b'.
 *   - Does not insert extra terminators beyond standard CRLF required by RESP.
 *
 * Return value:
 *   0 on success; non-zero on failure (e.g., allocation/resize error in 'b').
 *
 * Notes:
 *   - The function does not take ownership of 'argv' memory.
 *   - The dynamic buffer 'b' grows as needed to accommodate the encoded data.
 */
int resp_encode_array(struct dbuf *b, const slice_t * argv, size_t argc);
