#include "codec/lz.h"

#include <assert.h>
#include <string.h>

#define HASH_SIZE (1u << PARC_LZ_HASH_BITS)
#define NO_POS UINT32_MAX

static uint32_t read32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4); /* byte order irrelevant: only compared to itself */
    return v;
}

static uint32_t hash4(uint32_t v)
{
    return (v * 2654435761u) >> (32 - PARC_LZ_HASH_BITS);
}

size_t parc_lz_greedy(const uint8_t *src, size_t n, parc_tok *toks,
                      uint32_t *htab)
{
    size_t nt = 0;

    assert(n <= PARC_LZ_MAX_BLOCK);
    memset(htab, 0xFF, HASH_SIZE * sizeof *htab); /* all NO_POS */

    size_t i = 0;
    while (i + PARC_LZ_MIN_MATCH <= n) {
        uint32_t four = read32(src + i);
        uint32_t h = hash4(four);
        uint32_t cand = htab[h];
        htab[h] = (uint32_t)i;

        if (cand == NO_POS || read32(src + cand) != four) {
            toks[nt].dist = 0;
            toks[nt].len_or_lit = src[i];
            ++nt;
            ++i;
            continue;
        }

        size_t len = PARC_LZ_MIN_MATCH;
        while (i + len < n && src[cand + len] == src[i + len])
            ++len;
        toks[nt].dist = (uint32_t)(i - cand);
        toks[nt].len_or_lit = (uint32_t)len;
        ++nt;

        /* index the positions the match skips so later data can refer
         * back into it */
        size_t stop = i + len;
        if (stop + PARC_LZ_MIN_MATCH > n)
            stop = n >= PARC_LZ_MIN_MATCH ? n - PARC_LZ_MIN_MATCH + 1 : 0;
        for (size_t p = i + 1; p < stop; ++p)
            htab[hash4(read32(src + p))] = (uint32_t)p;
        i += len;
    }
    while (i < n) {
        toks[nt].dist = 0;
        toks[nt].len_or_lit = src[i];
        ++nt;
        ++i;
    }
    return nt;
}

/* Per-level matcher parameters. Level 1 is the greedy matcher (max_chain 0);
 * 2..9 deepen the chain search and raise the "good enough" nice_len. */
parc_lz_cfg parc_lz_cfg_for_level(unsigned level)
{
    static const parc_lz_cfg tbl[PARC_LZ_LEVEL_MAX + 1] = {
        {0, 0},         /* unused: level 0 resolves to a default upstream */
        {0, 0},         /* 1: greedy */
        {8, 32},        /* 2 */
        {16, 64},       /* 3 */
        {32, 64},       /* 4 */
        {64, 128},      /* 5 */
        {128, 256},     /* 6 */
        {256, 512},     /* 7 */
        {1024, 1024},   /* 8 */
        {4096, 4096},   /* 9 */
    };
    if (level < 1)
        level = 1;
    else if (level > PARC_LZ_LEVEL_MAX)
        level = PARC_LZ_LEVEL_MAX;
    return tbl[level];
}

/* Longest match for position i against the chain starting at cand (all chain
 * positions are < i). Returns the match length (>= PARC_LZ_MIN_MATCH) and its
 * distance in *dist, or 0 if no match of at least the minimum length exists. */
static uint32_t longest_match(const uint8_t *src, size_t n, size_t i,
                              uint32_t cand, const uint32_t *prev,
                              parc_lz_cfg cfg, uint32_t *dist)
{
    size_t max_len = n - i;
    size_t best = PARC_LZ_MIN_MATCH - 1; /* only >= MIN_MATCH counts */
    uint32_t best_dist = 0;
    uint32_t chain = cfg.max_chain;

    while (cand != NO_POS && chain--) {
        /* best < max_len always holds here (we break when best reaches
         * max_len), so src[i + best] and src[cand + best] are in bounds. */
        if (src[cand + best] == src[i + best]) {
            size_t l = 0;
            while (l < max_len && src[cand + l] == src[i + l])
                ++l;
            if (l > best) {
                best = l;
                best_dist = (uint32_t)(i - cand);
                if (best >= cfg.nice_len || best >= max_len)
                    break;
            }
        }
        cand = prev[cand];
    }
    if (best < PARC_LZ_MIN_MATCH)
        return 0;
    *dist = best_dist;
    return (uint32_t)best;
}

size_t parc_lz_chain(const uint8_t *src, size_t n, parc_tok *toks,
                     uint32_t *head, uint32_t *prev, parc_lz_cfg cfg)
{
    size_t nt = 0;

    assert(n <= PARC_LZ_MAX_BLOCK && cfg.max_chain >= 1);
    memset(head, 0xFF, HASH_SIZE * sizeof *head); /* all NO_POS */

    /* prev_len/prev_dist describe the match found at position s-1, deferred
     * one step so a longer match starting at s can supersede it (lazy). */
    uint32_t prev_len = 0, prev_dist = 0;
    int deferred = 0; /* a token for position s-1 is pending */
    size_t s = 0;

    while (s + PARC_LZ_MIN_MATCH <= n) {
        uint32_t h = hash4(read32(src + s));
        uint32_t cand = head[h];
        prev[s] = cand;
        head[h] = (uint32_t)s;

        uint32_t cur_len = 0, cur_dist = 0;
        if (cand != NO_POS && prev_len < cfg.nice_len)
            cur_len = longest_match(src, n, s, cand, prev, cfg, &cur_dist);

        if (deferred && prev_len >= PARC_LZ_MIN_MATCH && prev_len >= cur_len) {
            /* commit the match at s-1; s-1..end-1 are consumed. s-1 and s are
             * already in the chain; insert the rest of the matched span so
             * later positions can reference into it. */
            toks[nt].dist = prev_dist;
            toks[nt].len_or_lit = prev_len;
            ++nt;
            size_t end = (s - 1) + prev_len;
            for (size_t p = s + 1; p < end && p + PARC_LZ_MIN_MATCH <= n; ++p) {
                uint32_t hp = hash4(read32(src + p));
                prev[p] = head[hp];
                head[hp] = (uint32_t)p;
            }
            s = end;
            deferred = 0;
            prev_len = 0;
        } else {
            if (deferred) {
                toks[nt].dist = 0;
                toks[nt].len_or_lit = src[s - 1];
                ++nt;
            }
            prev_len = cur_len;
            prev_dist = cur_dist;
            deferred = 1;
            ++s;
        }
    }

    /* flush the deferred position (if any) and the tail as literals */
    for (size_t p = deferred ? s - 1 : s; p < n; ++p) {
        toks[nt].dist = 0;
        toks[nt].len_or_lit = src[p];
        ++nt;
    }
    return nt;
}
