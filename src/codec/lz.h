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

/* Matcher configuration for a compression level. max_chain == 0 selects the
 * greedy single-candidate matcher (parc_lz_greedy); otherwise the hash-chain
 * lazy matcher (parc_lz_chain) walks up to max_chain candidates per position
 * and stops searching once a match reaches nice_len bytes. */
typedef struct parc_lz_cfg {
    uint32_t max_chain;
    uint32_t nice_len;
} parc_lz_cfg;

enum {
    PARC_LZ_LEVEL_MAX = 9,
};

/* Map a compression level to a matcher config. level is clamped to
 * [1, PARC_LZ_LEVEL_MAX]. */
parc_lz_cfg parc_lz_cfg_for_level(unsigned level);

/* Tokenize src[0..n), n <= PARC_LZ_MAX_BLOCK. toks must hold n entries
 * (worst case: all literals); htab must hold 1 << PARC_LZ_HASH_BITS
 * entries and needs no initialization. Returns the token count.
 * Deterministic: same input always yields the same tokens. */
size_t parc_lz_greedy(const uint8_t *src, size_t n, parc_tok *toks,
                      uint32_t *htab);

/* Hash-chain matcher with one-step lazy evaluation. head holds
 * 1 << PARC_LZ_HASH_BITS entries and needs no initialization; prev holds n
 * entries (position -> earlier position with the same hash) and needs none
 * either. cfg.max_chain must be >= 1. Same token contract and determinism
 * as parc_lz_greedy; produces the same block-local, decoder-valid tokens,
 * just better matches. Returns the token count. */
size_t parc_lz_chain(const uint8_t *src, size_t n, parc_tok *toks,
                     uint32_t *head, uint32_t *prev, parc_lz_cfg cfg);

#ifdef __cplusplus
}
#endif

#endif /* PARC_CODEC_LZ_H */
