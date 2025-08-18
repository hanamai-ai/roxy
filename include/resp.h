#pragma once
#include "roxy.h"
#include "dbuf.h"
#include <stddef.h>

int resp_parse_request(const char *buf, size_t len, size_t *frame_len, struct roxy_cmd *out);
int resp_encode_array(struct dbuf *b, const slice_t *argv, size_t argc);
