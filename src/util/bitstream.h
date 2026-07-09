#ifndef PARC_UTIL_BITSTREAM_H
#define PARC_UTIL_BITSTREAM_H

#include <stddef.h>
#include <stdint.h>

#include "parc/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LSB-first bitstream over a caller-owned byte buffer.
 *
 * Bit order is part of the parc format and is pinned: successive fields
 * occupy increasingly significant bits of each byte, then successive bytes.
 * Writing 1 (1 bit), then 2 (2 bits), then 0x13 (5 bits) produces the single
 * byte 0x9D. The final byte is zero-padded in its unused high bits.
 *
 * Error handling is sticky: on the first failure (writer: out of capacity;
 * reader: read past end) the stream is marked failed, subsequent operations
 * are no-ops (reader returns 0), and the error surfaces from
 * parc_bw_finish() / parc_br_err(). This keeps hot loops branch-light:
 * callers may batch many puts/gets and check once. */

enum { PARC_BITSTREAM_MAX_BITS = 57 }; /* max n for a single put/get */

/* ---- writer ---- */

typedef struct parc_bw {
    uint8_t *dst;
    size_t cap;    /* capacity of dst in bytes */
    size_t pos;    /* bytes fully flushed to dst */
    uint64_t acc;  /* pending bits, LSB = oldest */
    unsigned nbits; /* number of pending bits in acc, < 8 after each put */
    int failed;
} parc_bw;

/* dst may be NULL iff cap == 0. */
void parc_bw_init(parc_bw *w, uint8_t *dst, size_t cap);

/* Append the n lowest bits of bits, n in [0, PARC_BITSTREAM_MAX_BITS];
 * n == 0 is a no-op. Requires bits < 2^n (higher bits clear); debug-assert,
 * not masked. */
void parc_bw_put(parc_bw *w, uint64_t bits, unsigned n);

/* Flush pending bits (zero-padding the last byte) and return PARC_OK with
 * *out_bytes = total bytes written, or PARC_ERR_LIMIT if capacity was ever
 * exceeded (*out_bytes = 0). The writer may not be used afterwards except to
 * re-init. out_bytes must be non-NULL. */
parc_err parc_bw_finish(parc_bw *w, size_t *out_bytes);

/* ---- reader ---- */

typedef struct parc_br {
    const uint8_t *src;
    size_t len;    /* total bytes available */
    size_t pos;    /* bytes consumed into acc */
    uint64_t acc;
    unsigned nbits;
    int failed;
} parc_br;

/* src may be NULL iff len == 0. */
void parc_br_init(parc_br *r, const uint8_t *src, size_t len);

/* Read n bits, n in [0, PARC_BITSTREAM_MAX_BITS]; n == 0 returns 0. Reading
 * beyond len*8 total bits marks the stream failed and returns 0. (Zero
 * padding bits in the final byte are readable like any others; consuming
 * them is not an error.) */
uint64_t parc_br_get(parc_br *r, unsigned n);

/* PARC_OK, or PARC_ERR_TRUNCATED if any get overran the buffer. */
parc_err parc_br_err(const parc_br *r);

/* Total bits consumed so far by successful gets. */
uint64_t parc_br_bits_consumed(const parc_br *r);

#ifdef __cplusplus
}
#endif

#endif /* PARC_UTIL_BITSTREAM_H */
