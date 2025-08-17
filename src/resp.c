#include "resp.h"
#include <string.h>
#include <limits.h>
#include <ctype.h>

static void str_to_upper(char *s) {
    for (; *s; s++) {
        *s = (char) toupper((unsigned char)*s);
    }
}

static long parse_crlf_number(const char *s, size_t n, size_t *digits_out) {
    size_t i=0; bool neg=false; if (i<n && s[i]=='-') {neg=true; i++;}
    long val=0;
    for (; i<n; ++i) {
        const char c=s[i];
        if (c=='\r') {
            if (i+1<n && s[i+1]=='\n') {
            *digits_out = i+2;
            return neg? -val: val;
        }
            return LONG_MIN;
        }
        if (c<'0'||c>'9')
            return LONG_MIN;
        val = val*10 + (c-'0');
    }
    return LONG_MIN;
}

int resp_parse_request(const char *buf, const size_t len, size_t *frame_len, struct roxy_cmd *out) {
    if (len==0)
        return 0;
    if (buf[0] == '*') {
        size_t pos = 0;
        size_t used;
        const long num_items = parse_crlf_number(buf+1, len-1, &used);
        if (num_items == LONG_MIN) return 0;
        if (num_items <= 0 || num_items > ROXY_MAX_ARGS) return -1;
        pos = 1 + used;
        out->argc = 0;
        for (long i=0; i<num_items; ++i) {
            if (pos >= len) return 0;
            if (buf[pos] != '$') return -1;
            size_t u2;
            long blen = parse_crlf_number(buf+pos+1, len-pos-1, &u2);
            if (blen == LONG_MIN) return 0;
            if (blen < 0) return -1;
            const size_t hdr = 1 + u2;
            if (pos + hdr + (size_t)blen + 2 > len) return 0;
            const char *b = buf + pos + hdr;
            out->argv[out->argc].ptr = b;
            out->argv[out->argc].len = (size_t)blen;
            out->argc++;
            pos += hdr + (size_t)blen + 2;
        }
        const size_t n0 = out->argv[0].len;
        const size_t copy = n0 < sizeof(out->cmd)-1 ? n0 : sizeof(out->cmd)-1;
        memcpy(out->cmd, out->argv[0].ptr, copy);
        out->cmd[copy] = '\0';
        str_to_upper(out->cmd);
        *frame_len = pos;
        return 1;
    }

    const char *cr = nullptr;
    for (size_t i=0;i<len-1;i++) { if (buf[i]=='\r' && buf[i+1]=='\n') { cr = buf+i; break; } }
    if (!cr) return 0;
    out->argc = 0;
    size_t i=0; while (&buf[i] < cr && out->argc < ROXY_MAX_ARGS) {
        while (&buf[i] < cr && (buf[i]==' '||buf[i]=='\t')) i++;
        const size_t start = i;
        while (&buf[i] < cr && buf[i]!=' ' && buf[i]!='\t') i++;
        if (i>start) {
            out->argv[out->argc].ptr = buf+start;
            out->argv[out->argc].len = (size_t)(i-start);
            out->argc++;
        }
    }
    if (out->argc==0) return -1;
    const size_t n0 = out->argv[0].len;
    const size_t copy = n0 < sizeof(out->cmd)-1 ? n0 : sizeof(out->cmd)-1;
    memcpy(out->cmd, out->argv[0].ptr, copy);
    out->cmd[copy]='\0'; str_to_upper(out->cmd);
    *frame_len = (size_t)((cr-buf)+2);
    return 1;
}

int resp_encode_array(struct dbuf *b, const slice_t *argv, size_t argc) {
    char hdr[64];
    int n = snprintf(hdr, sizeof(hdr), "*%zu\r\n", argc);
    if (n<0)
        return -1;
    if (dbuf_append(b, hdr, (size_t)n)<0)
        return -1;
    for (size_t i=0; i<argc; i++) {
        n = snprintf(hdr, sizeof(hdr), "$%zu\r\n", argv[i].len);
        if (n<0)
            return -1;
        if (dbuf_append(b, hdr, (size_t)n)<0)
            return -1;
        if (dbuf_append(b, argv[i].ptr, argv[i].len)<0)
            return -1;
        if (dbuf_append(b, "\r\n", 2)<0)
            return -1;
    }
    return 0;
}
