#ifndef PARC_CODEC_LZ_H
#define PARC_CODEC_LZ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Greedy hash-table LZ77 matcher (format-agnostic: produces tokens, does
 * not touch the bitstream). Matches are strictly block-local per
 * docs/FORMAT.md. */

enum {
    PARC_LZ_HASH_BITS = 15,
    PARC_LZ_MIN_MATCH = 4,
    PARC_LZ_MAX_BLOCK = 1u << 24, /* raw_len limit; distances fit 24 bits */
};

/* One token: dist == 0 means a literal byte in len_or_lit; otherwise a
 * match of length len_or_lit (>= PARC_LZ_MIN_MATCH) at distance dist. */
typedef struct parc_tok {
    uint32_t dist;
    uint32_t len_or_lit;
} parc_tok;

/* Tokenize src[0..n), n <= PARC_LZ_MAX_BLOCK. toks must hold n entries
 * (worst case: all literals); htab must hold 1 << PARC_LZ_HASH_BITS
 * entries and needs no initialization. Returns the token count.
 * Deterministic: same input always yields the same tokens. */
size_t parc_lz_greedy(const uint8_t *src, size_t n, parc_tok *toks,
                      uint32_t *htab);

#ifdef __cplusplus
}
#endif

#endif /* PARC_CODEC_LZ_H */
