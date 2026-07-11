#include "codec/block.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "codec/huffman.h"
#include "util/bitstream.h"

/* Alphabets per docs/FORMAT.md §2.2 */
#define MAIN_SYMS 282
#define DIST_SYMS 25
#define SYM_EOB 256
#define SYM_LEN0 257 /* first match-length bucket symbol */

/* bucket(v) per FORMAT.md §2.1: 0 for 0, else bit_length(v) */
static unsigned bucket(uint32_t v)
{
    return v == 0 ? 0 : 32u - (unsigned)__builtin_clz(v);
}

/* extra-bits payload of v within bucket b (call only for b >= 1) */
static uint32_t bucket_rem(uint32_t v, unsigned b)
{
    return v - (1u << (b - 1));
}

parc_err parc_blk_cctx_init(parc_blk_cctx *cx, size_t max_block,
                            unsigned level)
{
    if (max_block == 0 || max_block > PARC_LZ_MAX_BLOCK || level < 1 ||
        level > PARC_LZ_LEVEL_MAX)
        return PARC_ERR_ARG;
    cx->cfg = parc_lz_cfg_for_level(level);
    cx->htab = malloc(((size_t)1 << PARC_LZ_HASH_BITS) * sizeof *cx->htab);
    cx->toks = malloc(max_block * sizeof *cx->toks);
    cx->prev = cx->cfg.max_chain
                   ? malloc(max_block * sizeof *cx->prev)
                   : NULL;
    cx->max_block = max_block;
    if (!cx->htab || !cx->toks || (cx->cfg.max_chain && !cx->prev)) {
        parc_blk_cctx_free(cx);
        return PARC_ERR_NOMEM;
    }
    return PARC_OK;
}

void parc_blk_cctx_free(parc_blk_cctx *cx)
{
    free(cx->htab);
    free(cx->prev);
    free(cx->toks);
    cx->htab = NULL;
    cx->prev = NULL;
    cx->toks = NULL;
    cx->max_block = 0;
}

int parc_blk_compress(parc_blk_cctx *cx, const uint8_t *src, uint32_t raw_len,
                      uint8_t *dst, uint32_t *comp_len)
{
    assert(raw_len >= 1 && raw_len <= cx->max_block);

    size_t nt = cx->cfg.max_chain
                    ? parc_lz_chain(src, raw_len, cx->toks, cx->htab,
                                    cx->prev, cx->cfg)
                    : parc_lz_greedy(src, raw_len, cx->toks, cx->htab);

    uint32_t mfreq[MAIN_SYMS] = {0};
    uint32_t dfreq[DIST_SYMS] = {0};
    for (size_t t = 0; t < nt; ++t) {
        const parc_tok *tk = &cx->toks[t];
        if (tk->dist == 0) {
            mfreq[tk->len_or_lit]++;
        } else {
            mfreq[SYM_LEN0 + bucket(tk->len_or_lit - PARC_LZ_MIN_MATCH)]++;
            dfreq[bucket(tk->dist - 1)]++;
        }
    }
    mfreq[SYM_EOB]++;

    uint8_t mlens[MAIN_SYMS], dlens[DIST_SYMS];
    parc_huff_lens(mfreq, MAIN_SYMS, mlens);
    parc_huff_lens(dfreq, DIST_SYMS, dlens);
    parc_henc menc, denc;
    parc_henc_init(&menc, mlens, MAIN_SYMS);
    parc_henc_init(&denc, dlens, DIST_SYMS);

    /* capacity raw_len - 1 makes "packed must beat stored" automatic: any
     * overflow surfaces as PARC_ERR_LIMIT from the sticky writer */
    parc_bw w;
    parc_bw_init(&w, dst, raw_len - 1);
    for (unsigned s = 0; s < MAIN_SYMS; ++s)
        parc_bw_put(&w, mlens[s], 4);
    for (unsigned s = 0; s < DIST_SYMS; ++s)
        parc_bw_put(&w, dlens[s], 4);
    for (size_t t = 0; t < nt; ++t) {
        const parc_tok *tk = &cx->toks[t];
        if (tk->dist == 0) {
            parc_henc_put(&menc, &w, tk->len_or_lit);
            continue;
        }
        uint32_t lv = tk->len_or_lit - PARC_LZ_MIN_MATCH;
        unsigned lb = bucket(lv);
        parc_henc_put(&menc, &w, SYM_LEN0 + lb);
        if (lb >= 1)
            parc_bw_put(&w, bucket_rem(lv, lb), lb - 1);
        uint32_t dv = tk->dist - 1;
        unsigned db = bucket(dv);
        parc_henc_put(&denc, &w, db);
        if (db >= 1)
            parc_bw_put(&w, bucket_rem(dv, db), db - 1);
    }
    parc_henc_put(&menc, &w, SYM_EOB);

    size_t bytes = 0;
    if (parc_bw_finish(&w, &bytes) != PARC_OK) {
        *comp_len = raw_len;
        return PARC_BLK_STORED;
    }
    *comp_len = (uint32_t)bytes;
    return PARC_BLK_PACKED;
}

/* Read a bucketed value: bucket symbol b was already decoded; consume the
 * extra bits and return v. */
static uint32_t bucket_val(parc_br *r, unsigned b)
{
    if (b == 0)
        return 0;
    return (1u << (b - 1)) + (uint32_t)parc_br_get(r, b - 1);
}

parc_err parc_blk_decompress(const uint8_t *comp, uint32_t comp_len,
                             uint8_t *dst, uint32_t raw_len)
{
    parc_br r;
    parc_br_init(&r, comp, comp_len);

    uint8_t mlens[MAIN_SYMS], dlens[DIST_SYMS];
    for (unsigned s = 0; s < MAIN_SYMS; ++s)
        mlens[s] = (uint8_t)parc_br_get(&r, 4);
    for (unsigned s = 0; s < DIST_SYMS; ++s)
        dlens[s] = (uint8_t)parc_br_get(&r, 4);
    if (r.failed)
        return PARC_ERR_CORRUPT;

    parc_hdec mdec, ddec;
    if (parc_hdec_init(&mdec, mlens, MAIN_SYMS) != PARC_OK ||
        parc_hdec_init(&ddec, dlens, DIST_SYMS) != PARC_OK)
        return PARC_ERR_CORRUPT;
    if (mdec.nsyms == 0)
        return PARC_ERR_CORRUPT; /* EOB always occurs; main can't be empty */

    uint32_t pos = 0;
    for (;;) {
        int s = parc_hdec_get(&mdec, &r);
        if (s < 0 || r.failed)
            return PARC_ERR_CORRUPT;
        if (s < 256) {
            if (pos >= raw_len)
                return PARC_ERR_CORRUPT;
            dst[pos++] = (uint8_t)s;
            continue;
        }
        if (s == SYM_EOB)
            break;
        uint32_t len =
            bucket_val(&r, (unsigned)s - SYM_LEN0) + PARC_LZ_MIN_MATCH;
        if (ddec.nsyms == 0)
            return PARC_ERR_CORRUPT; /* match with no distance table */
        int ds = parc_hdec_get(&ddec, &r);
        if (ds < 0)
            return PARC_ERR_CORRUPT;
        uint32_t dist = bucket_val(&r, (unsigned)ds) + 1;
        if (r.failed)
            return PARC_ERR_CORRUPT;
        if (dist > pos || len > raw_len - pos)
            return PARC_ERR_CORRUPT;
        /* byte-by-byte: overlapping copies (dist < len) must repeat */
        for (uint32_t k = 0; k < len; ++k)
            dst[pos + k] = dst[pos + k - dist];
        pos += len;
    }

    if (pos != raw_len || r.failed)
        return PARC_ERR_CORRUPT;
    /* comp_len must be minimal and padding bits zero (§2) */
    uint64_t used = parc_br_bits_consumed(&r);
    if ((used + 7) / 8 != comp_len)
        return PARC_ERR_CORRUPT;
    unsigned pad = (unsigned)((uint64_t)comp_len * 8 - used);
    if (pad != 0 && parc_br_get(&r, pad) != 0)
        return PARC_ERR_CORRUPT;
    return PARC_OK;
}
