#include "util/rng.h"

/* TODO(subagent): implement per the contract in src/util/rng.h. */

void parc_rng_seed(parc_rng *r, uint64_t seed)
{
    (void)r;
    (void)seed;
}

uint64_t parc_rng_next(parc_rng *r)
{
    (void)r;
    return 0;
}

uint64_t parc_rng_range(parc_rng *r, uint64_t bound)
{
    (void)r;
    (void)bound;
    return 0;
}

double parc_rng_f64(parc_rng *r)
{
    (void)r;
    return 0.0;
}

void parc_rng_fill(parc_rng *r, void *dst, size_t len)
{
    (void)r;
    (void)dst;
    (void)len;
}
