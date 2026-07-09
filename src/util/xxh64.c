#include "util/xxh64.h"

/* TODO(subagent): implement per the contract in src/util/xxh64.h. */

uint64_t parc_xxh64(const void *data, size_t len, uint64_t seed)
{
    (void)data;
    (void)len;
    (void)seed;
    return 0;
}

void parc_xxh64_init(parc_xxh64_state *st, uint64_t seed)
{
    (void)st;
    (void)seed;
}

void parc_xxh64_update(parc_xxh64_state *st, const void *data, size_t len)
{
    (void)st;
    (void)data;
    (void)len;
}

uint64_t parc_xxh64_digest(const parc_xxh64_state *st)
{
    (void)st;
    return 0;
}
