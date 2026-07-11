#ifndef PARC_UTIL_BITSTREAM_H
#define PARC_UTIL_BITSTREAM_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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
 * callers may batch many puts/gets and check once.
 *
 * The per-symbol hot paths (parc_bw_put / parc_br_get / parc_br_peek) are
 * `static inline` here so the compiler keeps acc/nbits in registers across
 * consecutive puts/gets in the same caller, in every build (not only under
 * LTO). Everything hot in the codec funnels through them. */

enum { PARC_BITSTREAM_MAX_BITS = 57 }; /* max n for a single put/get */

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define PARC_BS_LITTLE_ENDIAN 1
#else
#define PARC_BS_LITTLE_ENDIAN 0
#endif

/* Read a little-endian 64-bit word via memcpy (no unaligned dereference, no
 * strict-aliasing violation). Matches the LSB-first byte order the reader
 * assembles one byte at a time, so a wide load is bit-identical to the byte
 * loop it replaces. */
static inline uint64_t parc_bs_read_le64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, sizeof v);
#if !PARC_BS_LITTLE_ENDIAN
    v = ((v & 0x00000000000000FFULL) << 56) |
        ((v & 0x000000000000FF00ULL) << 40) |
        ((v & 0x0000000000FF0000ULL) << 24) |
        ((v & 0x00000000FF000000ULL) << 8)  |
        ((v & 0x000000FF00000000ULL) >> 8)  |
        ((v & 0x0000FF0000000000ULL) >> 24) |
        ((v & 0x00FF000000000000ULL) >> 40) |
        ((v & 0xFF00000000000000ULL) >> 56);
#endif
    return v;
}

/* Store acc as a little-endian 64-bit word via memcpy. The byte-at-a-time
 * writer emits acc's byte 0 first, then byte 1, ... (LSB-first), so a
 * little-endian store of acc is bit-identical to that loop. Symmetric to
 * parc_bs_read_le64. */
static inline void parc_bs_write_le64(uint8_t *p, uint64_t v)
{
#if !PARC_BS_LITTLE_ENDIAN
    v = ((v & 0x00000000000000FFULL) << 56) |
        ((v & 0x000000000000FF00ULL) << 40) |
        ((v & 0x0000000000FF0000ULL) << 24) |
        ((v & 0x00000000FF000000ULL) << 8)  |
        ((v & 0x000000FF00000000ULL) >> 8)  |
        ((v & 0x0000FF0000000000ULL) >> 24) |
        ((v & 0x00FF000000000000ULL) >> 40) |
        ((v & 0xFF00000000000000ULL) >> 56);
#endif
    memcpy(p, &v, sizeof v);
}

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
static inline void parc_bw_put(parc_bw *w, uint64_t bits, unsigned n)
{
    assert(n <= PARC_BITSTREAM_MAX_BITS);
    assert(n == 0 || (bits >> n) == 0); /* higher bits must be clear */

    if (n == 0)
        return;
    if (w->failed)
        return;

    /* nbits < 8 before this call and n <= 57, so nbits + n <= 64: the shift
     * below never loses bits. */
    w->acc |= bits << w->nbits;
    w->nbits += n;

    /* Fast path: away from the tail (pos + 8 <= cap) drain every pending whole
     * byte with one little-endian 64-bit store. acc holds nbits <= 64 valid
     * bits, so whole = nbits >> 3 is in [1, 8]; we store all 8 acc bytes but
     * only advance pos by `whole`, leaving the residual < 8 bits in acc for the
     * next put (the extra stored bytes sit at uncommitted indices and are
     * overwritten by later puts or ignored — pos is the authoritative length).
     * Bit-identical to the byte loop it replaces. */
    if (w->nbits >= 8) {
        if (w->pos + 8 <= w->cap) {
            unsigned whole = w->nbits >> 3;
            unsigned shift = whole * 8u;
            parc_bs_write_le64(w->dst + w->pos, w->acc);
            w->pos += whole;
            w->acc = shift >= 64 ? 0 : (w->acc >> shift);
            w->nbits -= shift;
        } else {
            /* Tail: byte-at-a-time with a bounds check per byte. */
            while (w->nbits >= 8) {
                uint8_t byte = (uint8_t)(w->acc & 0xFFu);
                if (w->pos < w->cap) {
                    w->dst[w->pos] = byte;
                    w->pos++;
                } else {
                    w->failed = 1;
                }
                w->acc >>= 8;
                w->nbits -= 8;
            }
        }
    }
}

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

/* Ensure at least n (<= 57) bits are buffered in acc, or as many as remain in
 * the buffer if it runs dry (peek relies on the short fill; get pre-checks
 * that enough bits exist). Loading raises pos and nbits together, so
 * bits_consumed (pos*8 - nbits) is unchanged and bits past the end stay 0 in
 * acc's high positions.
 *
 * Fast path: away from the tail (pos + 8 <= len) one unaligned 64-bit load
 * tops acc up to >= 57 bits in a single step. Only whole bytes are merged
 * (take = floor((64 - nbits) / 8)), so acc's valid width stays a byte-exact
 * function of pos and the model never pulls a partial byte. Since we only fill
 * when nbits < n <= 57, nbits <= 56 here, so take >= 1 (progress guaranteed)
 * and nbits ends in [57, 64] (>= n). The re-read of the unconsumed high bytes
 * on the next fill is a load, not a correctness issue. */
static inline void parc_br_fill(parc_br *r, unsigned n)
{
    if (r->nbits >= n)
        return;

    if (r->pos + 8 <= r->len) {
        uint64_t word = parc_bs_read_le64(r->src + r->pos);
        unsigned take = (64u - r->nbits) >> 3; /* whole bytes to merge, 1..8 */
        if (take < 8)
            word &= (UINT64_C(1) << (take * 8)) - 1;
        r->acc |= word << r->nbits;
        r->pos += take;
        r->nbits += take * 8;
        return;
    }

    /* Tail: fewer than 8 bytes remain; merge them one at a time. */
    while (r->nbits < n && r->pos < r->len) {
        r->acc |= (uint64_t)r->src[r->pos] << r->nbits;
        r->pos++;
        r->nbits += 8;
    }
}

/* Read n bits, n in [0, PARC_BITSTREAM_MAX_BITS]; n == 0 returns 0. Reading
 * beyond len*8 total bits marks the stream failed and returns 0. (Zero
 * padding bits in the final byte are readable like any others; consuming
 * them is not an error.) */
static inline uint64_t parc_br_get(parc_br *r, unsigned n)
{
    assert(n <= PARC_BITSTREAM_MAX_BITS);

    if (n == 0)
        return 0;
    if (r->failed)
        return 0;

    uint64_t avail = (uint64_t)r->nbits + (uint64_t)(r->len - r->pos) * 8u;
    if (avail < n) {
        r->failed = 1;
        return 0;
    }

    parc_br_fill(r, n);

    uint64_t mask = (UINT64_C(1) << n) - 1;
    uint64_t val = r->acc & mask;
    r->acc >>= n;
    r->nbits -= n;
    return val;
}

/* Return the next n bits (LSB-first) without consuming them, n in
 * [0, PARC_BITSTREAM_MAX_BITS]. Bits past the end of the buffer read as 0 and
 * do NOT mark the stream failed (a peek never fails); consume the ones you
 * use with parc_br_get, which fails if they run past the end. Returns 0 if
 * the stream is already failed. Intended for length-prefixed table decode:
 * peek the maximum code width, look up the true width, then get it. */
static inline uint64_t parc_br_peek(parc_br *r, unsigned n)
{
    assert(n <= PARC_BITSTREAM_MAX_BITS);

    if (n == 0 || r->failed)
        return 0;

    parc_br_fill(r, n);
    return r->acc & ((UINT64_C(1) << n) - 1);
}

/* PARC_OK, or PARC_ERR_TRUNCATED if any get overran the buffer. */
parc_err parc_br_err(const parc_br *r);

/* Total bits consumed so far by successful gets. */
uint64_t parc_br_bits_consumed(const parc_br *r);

#ifdef __cplusplus
}
#endif

#endif /* PARC_UTIL_BITSTREAM_H */
