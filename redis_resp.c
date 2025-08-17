// redis_resp.c
// Tiny RESP3 parser + command mapper for Redis-like requests.
// CC0 / Public Domain.

#include "redis_resp.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

/*================== Small utils ==================*/

typedef struct {
    const char *p;
    const char *end;
    int need_more; // set to 1 when we determine the input is incomplete
} RSpan;

static int ci_equal_n(const char *a, size_t na, const char *b) {
    if (strlen(b) != na) return 0;
    for (size_t i=0; i<na; ++i) {
        if ((unsigned char)tolower(a[i]) != (unsigned char)tolower(b[i])) return 0;
    }
    return 1;
}

static int avail(RSpan *r, size_t n) {
    size_t have = (size_t)(r->end - r->p);
    if (have < n) { r->need_more = 1; return 0; }
    return 1;
}

static int read_crlf(RSpan *r) {
    if (!avail(r, 2)) return 0;
    if (r->p[0] == '\r' && r->p[1] == '\n') { r->p += 2; return 1; }
    return -1; // present but not CRLF -> error
}

static int find_crlf(const char *p, const char *end, const char **cr) {
    for (const char *q = p; q + 1 < end; ++q) {
        if (q[0] == '\r' && q[1] == '\n') { *cr = q; return 1; }
    }
    return 0; // not found
}

static int read_line(RSpan *r, const char **start, size_t *len) {
    const char *cr;
    if (!find_crlf(r->p, r->end, &cr)) { r->need_more = 1; return 0; }
    *start = r->p;
    *len   = (size_t)(cr - r->p);
    r->p   = cr + 2;
    return 1;
}

static int parse_i64_tok(const char *s, size_t n, long long *out) {
    char buf[64];
    if (n >= sizeof(buf)) return 0;
    memcpy(buf, s, n); buf[n]='\0';
    char *end=0; errno=0;
    long long v = strtoll(buf, &end, 10);
    if (errno || end==buf || *end!='\0') return 0;
    *out = v; return 1;
}

static int parse_double_tok(const char *s, size_t n, double *out) {
    if (n==3 && (strncasecmp(s,"inf",3)==0))  { *out = 1.0/0.0; return 1; }
    if (n==4 && (strncasecmp(s,"-inf",4)==0)) { *out = -1.0/0.0; return 1; }
    if (n==3 && (strncasecmp(s,"nan",3)==0))  { *out = 0.0/0.0; return 1; }
    char buf[96];
    if (n >= sizeof(buf)) return 0;
    memcpy(buf, s, n); buf[n]='\0';
    char *end=0; errno=0;
    double v = strtod(buf, &end);
    if (errno || end==buf || *end!='\0') return 0;
    *out = v; return 1;
}

static char *dup_nul_terminated(const char *s, size_t n) {
    char *p = (char*)malloc(n+1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n]='\0';
    return p;
}

/*================== Forward decls ==================*/
typedef struct Resp3Value Resp3Value;
static Resp3Value *parse_value(RSpan *r);
static Resp3Value *parse_array_like(RSpan *r, Resp3Type as_type);
static Resp3Value *parse_map(RSpan *r, Resp3Type as_type);
static Resp3Value *parse_blob(RSpan *r, Resp3Type as_type);
static Resp3Value *parse_simple_line(RSpan *r, Resp3Type as_type);
static Resp3Value *parse_verbatim(RSpan *r);
static Resp3Value *parse_numberish(RSpan *r, Resp3Type as_type);
static Resp3Value *parse_doubleish(RSpan *r);
static Resp3Value *parse_boolean(RSpan *r);
static Resp3Value *parse_null(RSpan *r);
static Resp3Value *alloc_v(Resp3Type t);

/*================== Allocation / Free ==================*/

static Resp3Value *alloc_v(Resp3Type t) {
    Resp3Value *v = (Resp3Value*)calloc(1, sizeof(*v));
    if (v) v->type = t;
    return v;
}

static void free_map(Resp3Map *m);
static void free_value(Resp3Value *v) {
    if (!v) return;
    if (v->attrs) {
        free_map(v->attrs);
        free(v->attrs);
        v->attrs = NULL;
    }
    switch (v->type) {
        case RESP3_SIMPLE_STRING:
        case RESP3_BLOB_STRING:
        case RESP3_VERBATIM_STRING:
        case RESP3_SIMPLE_ERROR:
        case RESP3_BLOB_ERROR:
        case RESP3_BIGNUM:
            free(v->as.str.buf);
            break;
        case RESP3_ARRAY:
        case RESP3_SET:
        case RESP3_PUSH:
            for (size_t i=0;i<v->as.array.len;i++) free_value(v->as.array.elems[i]);
            free(v->as.array.elems);
            break;
        case RESP3_MAP:
        case RESP3_ATTRIBUTE:
            free_map(&v->as.map);
            break;
        default: break;
    }
    free(v);
}

static void free_map(Resp3Map *m) {
    if (!m) return;
    for (size_t i=0;i<m->len;i++) {
        free_value(m->keys[i]);
        free_value(m->vals[i]);
    }
    free(m->keys);
    free(m->vals);
}

void resp3_free(Resp3Value *v) { free_value(v); }

/*================== Parsers per type (span) ==================*/

static Resp3Value *parse_blob(RSpan *r, Resp3Type as_type) {
    if (!avail(r, 1)) return NULL;
    if (*r->p != '$' && *r->p != '!') return NULL;
    r->p++;
    const char *line; size_t n;
    if (!read_line(r, &line, &n)) return NULL; // incomplete if need_more set
    long long len;
    if (!parse_i64_tok(line, n, &len) || len < -1) return NULL;
    if (len == -1) {
        Resp3Value *v = alloc_v(RESP3_NULL);
        return v;
    }
    if (!avail(r, (size_t)len + 2)) return NULL;
    Resp3Value *v = alloc_v(as_type);
    if (!v) return NULL;
    v->as.str.buf = (char*)malloc((size_t)len + 1);
    if (!v->as.str.buf) { free(v); return NULL; }
    memcpy(v->as.str.buf, r->p, (size_t)len);
    v->as.str.buf[len] = '\0';
    v->as.str.len = (size_t)len;
    r->p += len;
    int cr = read_crlf(r);
    if (cr != 1) { free_value(v); return NULL; }
    return v;
}

static Resp3Value *parse_simple_line(RSpan *r, Resp3Type as_type) {
    if (!avail(r, 1)) return NULL;
    r->p++; // skip '+' or '-'
    const char *line; size_t n;
    if (!read_line(r, &line, &n)) return NULL;
    Resp3Value *v = alloc_v(as_type);
    if (!v) return NULL;
    v->as.str.buf = dup_nul_terminated(line, n);
    if (!v->as.str.buf) { free(v); return NULL; }
    v->as.str.len = n;
    return v;
}

static Resp3Value *parse_verbatim(RSpan *r) {
    if (!avail(r, 1) || *r->p != '=') return NULL;
    r->p++;
    const char *line; size_t n;
    if (!read_line(r, &line, &n)) return NULL;
    long long len;
    if (!parse_i64_tok(line, n, &len) || len < 0) return NULL;
    if (!avail(r, (size_t)len + 2)) return NULL;
    Resp3Value *v = alloc_v(RESP3_VERBATIM_STRING);
    if (!v) return NULL;
    v->as.str.buf = (char*)malloc((size_t)len + 1);
    if (!v->as.str.buf) { free(v); return NULL; }
    memcpy(v->as.str.buf, r->p, (size_t)len);
    v->as.str.buf[len] = '\0';
    v->as.str.len = (size_t)len;
    r->p += len;
    int cr = read_crlf(r);
    if (cr != 1) { free_value(v); return NULL; }
    return v;
}

static Resp3Value *parse_numberish(RSpan *r, Resp3Type as_type) {
    if (!avail(r, 1)) return NULL;
    r->p++;
    const char *line; size_t n;
    if (!read_line(r, &line, &n)) return NULL;
    Resp3Value *v = alloc_v(as_type);
    if (!v) return NULL;
    if (as_type == RESP3_NUMBER) {
        long long val;
        if (!parse_i64_tok(line, n, &val)) { free(v); return NULL; }
        v->as.number = val;
    } else { // bignum as string
        v->as.str.buf = dup_nul_terminated(line, n);
        if (!v->as.str.buf) { free(v); return NULL; }
        v->as.str.len = n;
    }
    return v;
}

static Resp3Value *parse_doubleish(RSpan *r) {
    if (!avail(r, 1)) return NULL;
    r->p++;
    const char *line; size_t n;
    if (!read_line(r, &line, &n)) return NULL;
    double dv;
    if (!parse_double_tok(line, n, &dv)) return NULL;
    Resp3Value *v = alloc_v(RESP3_DOUBLE);
    if (!v) return NULL;
    v->as.dval = dv;
    return v;
}

static Resp3Value *parse_boolean(RSpan *r) {
    if (!avail(r, 1)) return NULL;
    r->p++;
    const char *line; size_t n;
    if (!read_line(r, &line, &n)) return NULL;
    if (n != 1 || (line[0] != 't' && line[0] != 'f')) return NULL;
    Resp3Value *v = alloc_v(RESP3_BOOLEAN);
    if (!v) return NULL;
    v->as.boolean = (line[0]=='t');
    return v;
}

static Resp3Value *parse_null(RSpan *r) {
    if (!avail(r, 1)) return NULL;
    r->p++;
    int cr = read_crlf(r);
    if (cr != 1) return NULL;
    Resp3Value *v = alloc_v(RESP3_NULL);
    return v;
}

static Resp3Value *parse_array_like(RSpan *r, Resp3Type as_type) {
    if (!avail(r, 1)) return NULL;
    r->p++; // skip prefix
    const char *line; size_t n;
    if (!read_line(r, &line, &n)) return NULL;
    long long len;
    if (!parse_i64_tok(line, n, &len) || len < 0) return NULL;

    Resp3Value *v = alloc_v(as_type);
    if (!v) return NULL;
    v->as.array.len = (size_t)len;
    v->as.array.elems = (Resp3Value**)calloc((size_t)len, sizeof(Resp3Value*));
    if (len && !v->as.array.elems) { free(v); return NULL; }

    for (long long i=0;i<len;i++) {
        Resp3Value *elem = parse_value(r);
        if (!elem) {
            if (r->need_more) { free_value(v); return NULL; } // incomplete
            free_value(v); return NULL; // error
        }
        v->as.array.elems[i] = elem;
    }
    return v;
}

static Resp3Value *parse_map(RSpan *r, Resp3Type as_type) {
    if (!avail(r, 1)) return NULL;
    r->p++; // skip prefix
    const char *line; size_t n;
    if (!read_line(r, &line, &n)) return NULL;
    long long len;
    if (!parse_i64_tok(line, n, &len) || len < 0) return NULL;

    Resp3Value *v = alloc_v(as_type);
    if (!v) return NULL;
    v->as.map.len = (size_t)len;
    v->as.map.keys = (Resp3Value**)calloc((size_t)len, sizeof(Resp3Value*));
    v->as.map.vals = (Resp3Value**)calloc((size_t)len, sizeof(Resp3Value*));
    if ((len && (!v->as.map.keys || !v->as.map.vals))) { free_value(v); return NULL; }

    for (long long i=0;i<len;i++) {
        Resp3Value *k = parse_value(r);
        if (!k) { if (r->need_more) { free_value(v); return NULL; } free_value(v); return NULL; }
        Resp3Value *val = parse_value(r);
        if (!val) { free_value(k); if (r->need_more) { free_value(v); return NULL; } free_value(v); return NULL; }
        v->as.map.keys[i] = k;
        v->as.map.vals[i] = val;
    }
    return v;
}

static Resp3Map *parse_optional_attributes(RSpan *r) {
    if (!avail(r, 1)) return NULL;
    if (*r->p != '|') return NULL;
    const char *save = r->p;
    Resp3Value *attr = parse_map(r, RESP3_ATTRIBUTE);
    if (!attr) {
        if (r->need_more) { r->p = save; } // rewind on incomplete
        return NULL;
    }
    Resp3Map *m = (Resp3Map*)malloc(sizeof(Resp3Map));
    if (!m) { free_value(attr); return NULL; }
    *m = attr->as.map; // steal contents
    free(attr);
    return m;
}

static Resp3Value *parse_value(RSpan *r) {
    Resp3Map *attrs = parse_optional_attributes(r);
    if (!avail(r, 1)) return NULL;

    Resp3Value *v = NULL;
    switch (*r->p) {
        case '+': v = parse_simple_line(r, RESP3_SIMPLE_STRING); break;
        case '-': v = parse_simple_line(r, RESP3_SIMPLE_ERROR);  break;
        case ':': v = parse_numberish   (r, RESP3_NUMBER);       break;
        case ',': v = parse_doubleish   (r);                     break;
        case '(': v = parse_numberish   (r, RESP3_BIGNUM);       break;
        case '#': v = parse_boolean     (r);                     break;
        case '_': v = parse_null        (r);                     break;
        case '$': v = parse_blob        (r, RESP3_BLOB_STRING);  break;
        case '!': v = parse_blob        (r, RESP3_BLOB_ERROR);   break;
        case '=': v = parse_verbatim    (r);                     break;
        case '*': v = parse_array_like  (r, RESP3_ARRAY);        break;
        case '~': v = parse_array_like  (r, RESP3_SET);          break;
        case '>': v = parse_array_like  (r, RESP3_PUSH);         break;
        case '%': v = parse_map         (r, RESP3_MAP);          break;
        default: return NULL;
    }
    if (!v) return NULL;
    v->attrs = attrs;
    return v;
}

/*================== Public helpers ==================*/

static int is_stringish(const Resp3Value *v) {
    return v && (v->type==RESP3_SIMPLE_STRING ||
                 v->type==RESP3_BLOB_STRING   ||
                 v->type==RESP3_VERBATIM_STRING);
}

RedisCommand redis_map_command(const char *s, size_t n) {
    if (!s) return CMD_UNKNOWN;
    if (ci_equal_n(s,n,"PING")) return CMD_PING;
    if (ci_equal_n(s,n,"ECHO")) return CMD_ECHO;
    if (ci_equal_n(s,n,"GET"))  return CMD_GET;
    if (ci_equal_n(s,n,"SET"))  return CMD_SET;
    if (ci_equal_n(s,n,"DEL"))  return CMD_DEL;
    if (ci_equal_n(s,n,"INCR")) return CMD_INCR;
    if (ci_equal_n(s,n,"DECR")) return CMD_DECR;
    if (ci_equal_n(s,n,"EXISTS")) return CMD_EXISTS;
    if (ci_equal_n(s,n,"EXPIRE")) return CMD_EXPIRE;
    if (ci_equal_n(s,n,"PUBLISH")) return CMD_PUBLISH;
    if (ci_equal_n(s,n,"SUBSCRIBE")) return CMD_SUBSCRIBE;
    if (ci_equal_n(s,n,"UNSUBSCRIBE")) return CMD_UNSUBSCRIBE;
    if (ci_equal_n(s,n,"LPUSH")) return CMD_LPUSH;
    if (ci_equal_n(s,n,"RPUSH")) return CMD_RPUSH;
    if (ci_equal_n(s,n,"LPOP"))  return CMD_LPOP;
    if (ci_equal_n(s,n,"RPOP"))  return CMD_RPOP;
    if (ci_equal_n(s,n,"SELECT"))return CMD_SELECT;
    if (ci_equal_n(s,n,"AUTH"))  return CMD_AUTH;
    if (ci_equal_n(s,n,"QUIT"))  return CMD_QUIT;
    return CMD_UNKNOWN;
}

int resp3_as_argv(const Resp3Value *v, const char ***argv, size_t *argc) {
    if (!v || (v->type != RESP3_ARRAY)) return 0;
    for (size_t i=0;i<v->as.array.len;i++) {
        const Resp3Value *e = v->as.array.elems[i];
        if (!is_stringish(e)) return 0;
    }
    *argc = v->as.array.len;
    const char **av = NULL;
    if (*argc) {
        av = (const char**)malloc(sizeof(char*) * (*argc));
        if (!av) return 0;
        for (size_t i=0;i<*argc;i++) {
            const Resp3Value *e = v->as.array.elems[i];
            av[i] = e->as.str.buf; // borrowed
        }
    }
    *argv = av;
    return 1;
}

/*================== Entrypoints ==================*/

Resp3ParseResult resp3_parse_command_span(const char *buf, size_t len,
                                          size_t *consumed, ParsedCommand *out) {
    if (!buf || !out || !consumed) return RESP3_PARSE_ERROR;
    *consumed = 0;
    RSpan r = { .p = buf, .end = buf + len, .need_more = 0 };

    const char *start = r.p;
    Resp3Value *root = parse_value(&r);
    if (!root) {
        if (r.need_more) return RESP3_PARSE_INCOMPLETE;
        return RESP3_PARSE_ERROR;
    }

    out->root = root;
    out->cmd  = CMD_UNKNOWN;
    if (root->type == RESP3_ARRAY && root->as.array.len > 0) {
        Resp3Value *first = root->as.array.elems[0];
        if (is_stringish(first) && first->as.str.buf) {
            out->cmd = redis_map_command(first->as.str.buf, first->as.str.len);
        }
    }
    *consumed = (size_t)(r.p - start);
    return RESP3_PARSE_OK;
}

int resp3_parse_command(const char *input, ParsedCommand *out) {
    if (!input || !out) return 0;
    size_t len = strlen(input);
    size_t consumed = 0;
    Resp3ParseResult pr = resp3_parse_command_span(input, len, &consumed, out);
    return pr == RESP3_PARSE_OK ? 1 : 0;
}

