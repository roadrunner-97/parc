#include "util/bitstream.h"

/* The per-symbol hot paths (parc_bw_put / parc_br_get / parc_br_peek) and
 * their helpers now live as `static inline` in bitstream.h so every caller
 * inlines them. Only the cold lifecycle/query functions remain here. */

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

parc_err parc_br_err(const parc_br *r)
{
    return r->failed ? PARC_ERR_TRUNCATED : PARC_OK;
}

uint64_t parc_br_bits_consumed(const parc_br *r)
{
    return (uint64_t)r->pos * 8u - (uint64_t)r->nbits;
}
