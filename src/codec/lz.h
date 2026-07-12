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

/* Optimal-parse (parc_lz_optimal) working-set sizes. The cost-based DP runs
 * over the block one chunk at a time so its scratch is bounded regardless of
 * block size: PARC_OPT_CHUNK positions per pass. PARC_OPT_MATCHES caps the
 * match frontier collected per position; PARC_OPT_BUDGET caps how many short
 * match lengths are priced per position (the longest match is always priced
 * on top of that). */
enum {
    PARC_OPT_CHUNK = 1u << 18, /* DP window, positions */
    PARC_OPT_MATCHES = 64,     /* frontier entries kept per position */
    PARC_OPT_BUDGET = 64,      /* short lengths priced per position */
};

/* One token: dist == 0 means a literal byte in len_or_lit; otherwise a
 * match of length len_or_lit (>= PARC_LZ_MIN_MATCH) at distance dist. */
typedef struct parc_tok {
    uint32_t dist;
    uint32_t len_or_lit;
} parc_tok;

/* Matcher configuration for a compression level. max_chain == 0 selects the
 * greedy single-candidate matcher (parc_lz_greedy); otherwise a hash-chain
 * walk of up to max_chain candidates per position that stops once a match
 * reaches nice_len bytes. When optimal != 0 the chain feeds a cost-based
 * dynamic-programming parse (parc_lz_optimal); otherwise it feeds the
 * one-step-lazy parse (parc_lz_chain). */
typedef struct parc_lz_cfg {
    uint32_t max_chain;
    uint32_t nice_len;
    uint32_t optimal;
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

/* Cost-based optimal parse. head/prev are the same hash-chain arrays as
 * parc_lz_chain (head needs no init; prev holds n entries). price, bt_len and
 * bt_dist are DP scratch, each holding PARC_OPT_CHUNK + 1 entries; rep is DP
 * scratch holding 3 * (PARC_OPT_CHUNK + 1) entries (the recent-offset cache
 * carried along each path). None need initialization. cfg.max_chain must be
 * >= 1; cfg.optimal is ignored here (the caller has already dispatched). The
 * parse is repeat-offset-aware: it models the v1 3-entry recent-offset cache so
 * matches reusing a recent distance are priced cheap (no offset extra bits),
 * matching the sequence coder. Same token contract and determinism as the other
 * matchers: block-local, decoder-valid tokens that rebuild the input, chosen to
 * minimize an estimated bit cost. Returns the token count. */
size_t parc_lz_optimal(const uint8_t *src, size_t n, parc_tok *toks,
                       uint32_t *head, uint32_t *prev, parc_lz_cfg cfg,
                       uint64_t *price, uint32_t *bt_len, uint32_t *bt_dist,
                       uint32_t *rep);

#ifdef __cplusplus
}
#endif

#endif /* PARC_CODEC_LZ_H */
