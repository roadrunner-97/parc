#include "util/bitstream.h"

#include <assert.h>

/* ---- writer ---- */

void parc_bw_init(parc_bw *w, uint8_t *dst, size_t cap)
{
    w->dst = dst;
    w->cap = cap;
    w->pos = 0;
    w->acc = 0;
    w->nbits = 0;
    w->failed = 0;
}

void parc_bw_put(parc_bw *w, uint64_t bits, unsigned n)
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

parc_err parc_bw_finish(parc_bw *w, size_t *out_bytes)
{
    if (w->nbits > 0) {
        uint8_t byte = (uint8_t)(w->acc & 0xFFu);
        if (w->pos < w->cap) {
            w->dst[w->pos] = byte;
            w->pos++;
        } else {
            w->failed = 1;
        }
        w->acc = 0;
        w->nbits = 0;
    }

    if (w->failed) {
        *out_bytes = 0;
        return PARC_ERR_LIMIT;
    }

    *out_bytes = w->pos;
    return PARC_OK;
}

/* ---- reader ---- */

void parc_br_init(parc_br *r, const uint8_t *src, size_t len)
{
    r->src = src;
    r->len = len;
    r->pos = 0;
    r->acc = 0;
    r->nbits = 0;
    r->failed = 0;
}

uint64_t parc_br_get(parc_br *r, unsigned n)
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

    /* nbits < 8 on entry and n <= 57, so this can load at most 8 bytes,
     * keeping the top bit used (nbits-1+7 <= 63) within range. */
    while (r->nbits < n) {
        r->acc |= (uint64_t)r->src[r->pos] << r->nbits;
        r->pos++;
        r->nbits += 8;
    }

    uint64_t mask = (UINT64_C(1) << n) - 1;
    uint64_t val = r->acc & mask;
    r->acc >>= n;
    r->nbits -= n;
    return val;
}

parc_err parc_br_err(const parc_br *r)
{
    return r->failed ? PARC_ERR_TRUNCATED : PARC_OK;
}

uint64_t parc_br_bits_consumed(const parc_br *r)
{
    return (uint64_t)r->pos * 8u - (uint64_t)r->nbits;
}
