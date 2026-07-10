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
