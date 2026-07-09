#include "util/bitstream.h"

/* TODO(subagent): implement per the contract in src/util/bitstream.h. */

void parc_bw_init(parc_bw *w, uint8_t *dst, size_t cap)
{
    (void)w;
    (void)dst;
    (void)cap;
}

void parc_bw_put(parc_bw *w, uint64_t bits, unsigned n)
{
    (void)w;
    (void)bits;
    (void)n;
}

parc_err parc_bw_finish(parc_bw *w, size_t *out_bytes)
{
    (void)w;
    (void)out_bytes;
    return PARC_ERR_LIMIT;
}

void parc_br_init(parc_br *r, const uint8_t *src, size_t len)
{
    (void)r;
    (void)src;
    (void)len;
}

uint64_t parc_br_get(parc_br *r, unsigned n)
{
    (void)r;
    (void)n;
    return 0;
}

parc_err parc_br_err(const parc_br *r)
{
    (void)r;
    return PARC_ERR_TRUNCATED;
}

uint64_t parc_br_bits_consumed(const parc_br *r)
{
    (void)r;
    return 0;
}
