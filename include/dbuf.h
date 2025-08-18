// C dynamic byte buffer (dbuf) interface.
//
// Overview
// --------
// dbuf is a simple growable byte buffer for incremental I/O and message
// assembly. It keeps track of a readable region [rpos, wpos) within a
// heap-allocated array `data` whose total allocated size is `cap`.
//
// Typical usage pattern
// ---------------------
//   struct dbuf b;
//   dbuf_init(&b);
//
//   // Append bytes produced by your code
//   dbuf_append(&b, src, src_len);
//
//   // Read from a file descriptor into the buffer
//   dbuf_read_from_fd(&b, fd);
//
//   // Access readable data as a contiguous span:
//   //   pointer: b.data + b.rpos
//   //   length : dbuf_len(&b)
//
//   // Consume bytes once processed
//   dbuf_consume(&b, n);
//
//   dbuf_free(&b);
//
// Concurrency
// -----------
// Not thread-safe. External synchronization is required if shared across threads.
//
// Memory/ownership
// ----------------
// The buffer manages its own storage via dynamic allocation. Call dbuf_free()
// to release memory. After dbuf_free(), the buffer must be re-initialized
// with dbuf_init() before reuse.
//
// Error handling
// --------------
// Functions returning int typically return 0 on success and -1 on failure.
// Functions returning ssize_t typically return a non-negative byte count on
// success and -1 on failure. Where applicable, the implementation may set
// errno on failure.
//
// Invariants
// ----------
//   0 <= rpos <= wpos <= cap
//   dbuf_len(b) == wpos - rpos
//
// Data access notes
// -----------------
// - The readable slice is [data + rpos, data + wpos).
// - Appends extend wpos; consumes advance rpos.
// - Implementations may compact (shift) data when the readable region becomes
//   empty or to make room for appends. Do not keep raw pointers to b->data
//   across calls that may reallocate/compact (e.g., reserve/append/read).
#pragma once
#include <stddef.h>
#include <sys/types.h>

struct dbuf {
	char *data;		// Pointer to heap-allocated storage (may be NULL after init/free).
	size_t cap;		// Total allocated capacity of 'data' in bytes.
	size_t rpos;		// Read position (start offset of readable data).
	size_t wpos;		// Write position (one past the last written byte).
};

// Initialize an empty buffer.
// - Sets all fields to a valid empty state (no allocation required).
// - Must be called before any other operation on the buffer.
void dbuf_init(struct dbuf *b);

// Release any memory held by the buffer and reset it to the empty state.
// - Safe to call on an already-empty buffer.
// - After this call, the buffer can be reused by calling dbuf_init() again.
void dbuf_free(struct dbuf *b);

// Return the number of bytes currently readable in the buffer.
// - Equivalent to (b->wpos - b->rpos).
size_t dbuf_len(const struct dbuf *b);

// Ensure the buffer has at least 'extra' bytes of free space available after wpos.
// - May reallocate the underlying storage and/or compact existing data.
// - On success returns 0; on failure returns -1.
int dbuf_reserve(struct dbuf *b, size_t extra);

// Append 'n' bytes from 'data' to the end of the buffer.
// - Grows the buffer if needed (as if by dbuf_reserve()).
// - On success returns 0; on allocation failure returns -1.
int dbuf_append(struct dbuf *b, const void *data, size_t n);

// Consume (discard) the first 'n' readable bytes from the buffer.
// - Advances rpos by n, clamping at wpos.
// - Implementations may reset rpos/wpos to 0 when the buffer becomes empty
//   to avoid unbounded growth of indices.
void dbuf_consume(struct dbuf *b, size_t n);

// Read from a file descriptor into the buffer's free space, appending bytes.
// - Behavior mirrors read(2) semantics on the underlying fd.
// - May grow/compact the buffer to make room.
// - Returns number of bytes read (>= 0), 0 on EOF, or -1 on error.
ssize_t dbuf_read_from_fd(struct dbuf *b, int fd);

// Write the buffer's readable bytes to a file descriptor, consuming what is written.
// - Behavior mirrors write(2) semantics on the underlying fd.
// - May perform partial writes; in that case only the written bytes are consumed.
// - Returns number of bytes written (>= 0) or -1 on error.
ssize_t dbuf_write_to_fd(struct dbuf *b, int fd);
