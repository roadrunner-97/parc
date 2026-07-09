#ifndef PARC_UTIL_RNG_H
#define PARC_UTIL_RNG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Deterministic, seedable PRNG shared by the codec heuristics, parcgen and
 * the property tests. NOT cryptographic.
 *
 * Algorithm is pinned so streams are reproducible forever and across
 * platforms — do not change it:
 *   - State expansion: splitmix64 (Steele/Lea/Flood). Starting from the
 *     user seed x, four successive splitmix64 outputs fill s[0..3] in order.
 *     splitmix64 step: z = (x += 0x9E3779B97F4A7C15);
 *                      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9;
 *                      z = (z ^ (z >> 27)) * 0x94D049BB133111EB;
 *                      return z ^ (z >> 31);
 *   - Generator: xoshiro256** 1.0 (Blackman/Vigna, public domain):
 *       result = rotl64(s[1] * 5, 7) * 9;
 *       t = s[1] << 17;
 *       s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
 *       s[2] ^= t;    s[3] = rotl64(s[3], 45);
 * The test suite checks output equality against an embedded canonical
 * reference implementation. */
typedef struct parc_rng {
    uint64_t s[4];
} parc_rng;

/* Any seed value is valid, including 0. */
void parc_rng_seed(parc_rng *r, uint64_t seed);

/* Next 64 uniform bits. */
uint64_t parc_rng_next(parc_rng *r);

/* Uniform in [0, bound); bound == 0 returns 0 without consuming output.
 * Must be unbiased for every bound (use rejection, e.g. Lemire's method;
 * the exact rejection scheme is implementation-defined). */
uint64_t parc_rng_range(parc_rng *r, uint64_t bound);

/* Uniform double in [0, 1), pinned as: (parc_rng_next(r) >> 11) * 0x1.0p-53 */
double parc_rng_f64(parc_rng *r);

/* Fill dst with len bytes, pinned as: successive parc_rng_next() outputs
 * serialized little-endian; a trailing partial word uses its lowest bytes.
 * dst may be NULL iff len == 0. */
void parc_rng_fill(parc_rng *r, void *dst, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* PARC_UTIL_RNG_H */
