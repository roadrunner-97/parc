#include "util/rng.h"

#include <string.h>

/* splitmix64 (Steele/Lea/Flood), used only to expand the user seed into the
 * four xoshiro256** state words. */
static uint64_t splitmix64_next(uint64_t *x)
{
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static uint64_t rotl64(uint64_t x, unsigned k)
{
    return (x << k) | (x >> (64u - k));
}

void parc_rng_seed(parc_rng *r, uint64_t seed)
{
    uint64_t x = seed;
    for (size_t i = 0; i < 4; ++i)
        r->s[i] = splitmix64_next(&x);
}

uint64_t parc_rng_next(parc_rng *r)
{
    const uint64_t result = rotl64(r->s[1] * 5u, 7) * 9u;
    const uint64_t t = r->s[1] << 17;

    r->s[2] ^= r->s[0];
    r->s[3] ^= r->s[1];
    r->s[1] ^= r->s[2];
    r->s[0] ^= r->s[3];
    r->s[2] ^= t;
    r->s[3] = rotl64(r->s[3], 45);

    return result;
}

uint64_t parc_rng_range(parc_rng *r, uint64_t bound)
{
    if (bound == 0)
        return 0;

    /* Lemire's method: unbiased via rejection on the low 64 bits of the
     * 128-bit product x * bound. */
    uint64_t x = parc_rng_next(r);
    __uint128_t m = (__uint128_t)x * (__uint128_t)bound;
    uint64_t l = (uint64_t)m;
    if (l < bound) {
        uint64_t t = (uint64_t)(-bound) % bound;
        while (l < t) {
            x = parc_rng_next(r);
            m = (__uint128_t)x * (__uint128_t)bound;
            l = (uint64_t)m;
        }
    }
    return (uint64_t)(m >> 64);
}

double parc_rng_f64(parc_rng *r)
{
    return (double)(parc_rng_next(r) >> 11) * 0x1.0p-53;
}

void parc_rng_fill(parc_rng *r, void *dst, size_t len)
{
    uint8_t *out = (uint8_t *)dst;
    size_t i = 0;
    while (i < len) {
        uint64_t w = parc_rng_next(r);
        size_t n = (len - i < 8) ? (len - i) : 8;
        for (size_t j = 0; j < n; ++j)
            out[i + j] = (uint8_t)(w >> (8u * j));
        i += n;
    }
}
