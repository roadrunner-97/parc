#ifndef PARC_UTIL_XXH64_H
#define PARC_UTIL_XXH64_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* XXH64 (64-bit xxHash, Yann Collet). Digests MUST be bit-identical to the
 * reference implementation (github.com/Cyan4973/xxHash) for every input and
 * seed, independent of host endianness. Reference test vectors generated with
 * the official library live in tests/test_xxh64.cpp.
 *
 * Implementation notes: clean-room from the published XXH64 spec
 * (xxhash.com), standard constants PRIME64_1..5; input words are read
 * little-endian via memcpy (no unaligned dereference, no strict-aliasing
 * violations). */

/* One-shot hash of len bytes. data may be NULL iff len == 0. */
uint64_t parc_xxh64(const void *data, size_t len, uint64_t seed);

/* Streaming state: any sequence of updates whose concatenation equals a
 * buffer must digest to exactly parc_xxh64(buffer). No heap allocation. */
typedef struct parc_xxh64_state {
    uint64_t acc[4];
    uint64_t total_len;
    uint64_t seed;
    uint8_t buf[32];
    size_t buf_len;
} parc_xxh64_state;

void parc_xxh64_init(parc_xxh64_state *st, uint64_t seed);
void parc_xxh64_update(parc_xxh64_state *st, const void *data, size_t len);
/* Can be called at any point and does not modify *st (more updates may
 * follow). */
uint64_t parc_xxh64_digest(const parc_xxh64_state *st);

#ifdef __cplusplus
}
#endif

#endif /* PARC_UTIL_XXH64_H */
