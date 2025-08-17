// redis_resp.h
// Tiny RESP3 parser + command mapper for Redis-like requests.
// CC0 / Public Domain.

#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============== Commands (extend as you wish) ==============*/
typedef enum {
    CMD_PING,
    CMD_ECHO,
    CMD_GET,
    CMD_SET,
    CMD_DEL,
    CMD_INCR,
    CMD_DECR,
    CMD_EXISTS,
    CMD_EXPIRE,
    CMD_PUBLISH,
    CMD_SUBSCRIBE,
    CMD_UNSUBSCRIBE,
    CMD_LPUSH,
    CMD_RPUSH,
    CMD_LPOP,
    CMD_RPOP,
    CMD_SELECT,
    CMD_AUTH,
    CMD_QUIT,
    CMD_UNKNOWN
} RedisCommand;

/*============== RESP3 AST (minimal but useful) ==============*/
typedef enum {
    RESP3_SIMPLE_STRING,   // '+'
    RESP3_SIMPLE_ERROR,    // '-'
    RESP3_NUMBER,          // ':'
    RESP3_DOUBLE,          // ','
    RESP3_BIGNUM,          // '('
    RESP3_BOOLEAN,         // '#'
    RESP3_NULL,            // '_'
    RESP3_BLOB_STRING,     // '$'
    RESP3_BLOB_ERROR,      // '!'
    RESP3_VERBATIM_STRING, // '='
    RESP3_ARRAY,           // '*'
    RESP3_MAP,             // '%'
    RESP3_SET,             // '~'
    RESP3_PUSH,            // '>'
    RESP3_ATTRIBUTE        // '|'  (attached optionally)
} Resp3Type;

typedef struct Resp3Value Resp3Value;

typedef struct {
    char *buf;        // NUL-terminated copy of payload (binary-safe; length below)
    size_t len;       // exact length without CRLF
} Resp3String;

typedef struct {
    Resp3Value **elems;
    size_t len;
} Resp3Array;

typedef struct {
    Resp3Value **keys;
    Resp3Value **vals;
    size_t len;
} Resp3Map;

struct Resp3Value {
    Resp3Type type;
    union {
        Resp3String str;     // strings/errors/bignum/verbatim
        long long   number;  // integer
        double      dval;    // double
        int         boolean; // bool
        Resp3Array  array;   // array/set/push
        Resp3Map    map;     // map/attributes
    } as;
    Resp3Map *attrs;         // optional attributes (owned)
};

/*============== Command-oriented wrapper ==============*/
typedef struct {
    RedisCommand cmd; // enum mapped from argv[0] if top-level array of strings
    Resp3Value  *root; // full parsed tree (free with resp3_free)
} ParsedCommand;

/*============== Parse results ==============*/
typedef enum {
    RESP3_PARSE_OK = 0,
    RESP3_PARSE_INCOMPLETE = 1,  // need more bytes
    RESP3_PARSE_ERROR = 2        // malformed
} Resp3ParseResult;

/*============== API ==============*/

/** Existing convenience (NUL-terminated input). Returns 1 on success else 0. */
int resp3_parse_command(const char *input, ParsedCommand *out);

/**
 * New: span-based streaming parser.
 *
 * - Parses exactly one RESP3 frame from buf[0..len).
 * - On OK: returns RESP3_PARSE_OK, sets *consumed to the number of bytes used
 *          by the frame, fills *out (you own out->root; free with resp3_free).
 * - On INCOMPLETE: returns RESP3_PARSE_INCOMPLETE, *consumed==0, out untouched.
 * - On ERROR: returns RESP3_PARSE_ERROR, *consumed==0, out untouched.
 *
 * This enables robust pipelining: call in a loop, drop `consumed` bytes,
 * and try again on remaining data.
 */
Resp3ParseResult resp3_parse_command_span(const char *buf, size_t len,
                                          size_t *consumed, ParsedCommand *out);

/** Free the full RESP3 tree created by resp3_parse_command()/span(). */
void resp3_free(Resp3Value *v);

/** Case-insensitive mapper (public if you build commands elsewhere). */
RedisCommand redis_map_command(const char *s, size_t n);

/**
 * If v is an ARRAY of string-like elements, produce argv/argc.
 * - argv is a malloc’d vector of borrowed pointers (owned by v), free the vector.
 * - Returns 1 on success, 0 otherwise.
 */
int resp3_as_argv(const Resp3Value *v, const char ***argv, size_t *argc);

#ifdef __cplusplus
}
#endif

