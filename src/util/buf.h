#ifndef PARC_UTIL_BUF_H
#define PARC_UTIL_BUF_H

#include <stddef.h>
#include <stdint.h>

#include "parc/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Growable byte buffer. Zero-initialized state (all fields 0) is a valid
 * empty buffer. On any PARC_ERR_NOMEM the buffer is left unchanged and
 * remains valid. */
typedef struct parc_buf {
    uint8_t *data;
    size_t len;
    size_t cap;
} parc_buf;

/* Empty buffer, no allocation. */
void parc_buf_init(parc_buf *b);

/* Release storage and reset to the init state. Idempotent. */
void parc_buf_free(parc_buf *b);

/* Ensure cap >= want, preserving contents. Growth is geometric (at least
 * 1.5x the old capacity when growing), so N appends cost O(N) amortized. */
parc_err parc_buf_reserve(parc_buf *b, size_t want);

/* Append len bytes from src (may be NULL iff len == 0). src must not alias
 * b->data. */
parc_err parc_buf_append(parc_buf *b, const void *src, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* PARC_UTIL_BUF_H */
