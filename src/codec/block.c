#include "codec/block.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "codec/fse.h"
#include "codec/huffman.h"
#include "util/bitstream.h"
#include "util/prof.h"

/* v0 alphabets per docs/FORMAT.md §2.2 */
#define MAIN_SYMS 282
#define DIST_SYMS 25
#define SYM_EOB 256
#define SYM_LEN0 257 /* first match-length bucket symbol */

/* v1 sequence-model alphabets (§3). Literal-length and match-length are
 * value buckets (0..24). Offset codes: 0,1,2 are the repeat offsets, 3+b is a
 * new-offset bucket b of distance-1. */
#define LIT_SYMS 256
#define LL_SYMS 25
#define ML_SYMS 25
#define OF_SYMS 28
#define NREP 3 /* recent-offset cache depth */

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

/* Read a bucketed value: bucket symbol b was already decoded; consume the
 * extra bits and return v. */
static uint32_t bucket_val(parc_br *r, unsigned b)
{
    if (b == 0)
        return 0;
    return (1u << (b - 1)) + (uint32_t)parc_br_get(r, b - 1);
}

/* Copy `len` bytes from `s` to `d` in unconditional 16-byte chunks ("wildcopy"),
 * for NON-overlapping ranges only (d - s >= 16, or entirely disjoint). Writes
 * and reads up to 15 bytes past the logical end of the copy; callers guarantee
 * both buffers carry >= PARC_WILDCOPY_SLACK trailing bytes so the overrun stays
 * in-bounds. Avoids libc memcpy's length dispatch, which dominates LZ decode
 * where most copies are short (a few bytes). `len` may be 0 (still stores one
 * chunk of garbage into slack, harmless). */
static inline void wild_copy(uint8_t *d, const uint8_t *s, size_t len)
{
    uint8_t *end = d + len;
    do {
        memcpy(d, s, 16); /* single 16-byte vector move */
        d += 16;
        s += 16;
    } while (d < end);
}

/* Copy an LZ match: dst[pos+k] = dst[pos+k-dist] for k in [0, len). Callers
 * must have validated dist <= pos and pos + len <= raw_len, and `dst` carries
 * PARC_WILDCOPY_SLACK trailing bytes (so the wildcopy overrun is in-bounds).
 *
 * dist >= 16: source and destination stay >= 16 apart as both advance, so each
 * 16-byte chunk is disjoint and a straight wildcopy reproduces the run.
 * dist < 16: overlapping period; grow the written run by doubling (bulk memcpy
 * of disjoint prefixes). Copying from a fixed 16-byte-back source would only
 * reproduce the period when dist divides 16, so the doubling is kept for the
 * whole small-offset run. Small offsets are the minority of matches. */
static void copy_match(uint8_t *dst, uint32_t pos, uint32_t dist, uint32_t len)
{
    uint8_t *d = dst + pos;
    const uint8_t *s = d - dist;
    if (dist >= 16) {
        wild_copy(d, s, len);
        return;
    }
    /* Seed one period (adjacent, non-overlapping: s + dist == d), then double:
     * `filled` is always a multiple of dist here, so d[0..chunk) is a valid
     * prefix of the output and chunk <= filled keeps the ranges disjoint. */
    memcpy(d, s, dist);
    uint32_t filled = dist;
    while (filled < len) {
        uint32_t chunk = filled < len - filled ? filled : len - filled;
        memcpy(d + filled, d, chunk);
        filled += chunk;
    }
}

/* ---- context lifecycle ---- */

parc_err parc_blk_cctx_init(parc_blk_cctx *cx, size_t max_block, unsigned level,
                            unsigned version)
{
    if (max_block == 0 || max_block > PARC_LZ_MAX_BLOCK || level < 1 ||
        level > PARC_LZ_LEVEL_MAX || version > 2)
        return PARC_ERR_ARG;
    memset(cx, 0, sizeof *cx);
    cx->cfg = parc_lz_cfg_for_level(level);
    cx->version = version;
    cx->max_block = max_block;
    cx->htab = malloc(((size_t)1 << PARC_LZ_HASH_BITS) * sizeof *cx->htab);
    cx->toks = malloc(max_block * sizeof *cx->toks);
    cx->prev = cx->cfg.max_chain ? malloc(max_block * sizeof *cx->prev) : NULL;
    int ok = cx->htab && cx->toks && (!cx->cfg.max_chain || cx->prev);

    if (cx->cfg.optimal) {
        size_t nchunk = (size_t)PARC_OPT_CHUNK + 1;
        cx->opt_price = malloc(nchunk * sizeof *cx->opt_price);
        cx->opt_len = malloc(nchunk * sizeof *cx->opt_len);
        cx->opt_dist = malloc(nchunk * sizeof *cx->opt_dist);
        cx->opt_rep = malloc(3 * nchunk * sizeof *cx->opt_rep);
        cx->alt = malloc(max_block);
        ok = ok && cx->opt_price && cx->opt_len && cx->opt_dist &&
             cx->opt_rep && cx->alt;
    }

    if (version >= 1) { /* v1 and v2 share the sequence-model scratch */
        size_t nseq_max = max_block / PARC_LZ_MIN_MATCH + 1;
        cx->lit = malloc(max_block);
        cx->ll_sym = malloc(nseq_max);
        cx->ml_sym = malloc(nseq_max);
        cx->of_sym = malloc(nseq_max);
        cx->ll_ex = malloc(nseq_max * sizeof *cx->ll_ex);
        cx->ml_ex = malloc(nseq_max * sizeof *cx->ml_ex);
        cx->of_ex = malloc(nseq_max * sizeof *cx->of_ex);
        cx->grp = malloc(max_block * sizeof *cx->grp);
        cx->fenc = malloc(sizeof(parc_fenc));
        ok = ok && cx->lit && cx->ll_sym && cx->ml_sym && cx->of_sym &&
             cx->ll_ex && cx->ml_ex && cx->of_ex && cx->grp && cx->fenc;
    }
    if (!ok) {
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
    free(cx->opt_price);
    free(cx->opt_len);
    free(cx->opt_dist);
    free(cx->opt_rep);
    free(cx->alt);
    free(cx->lit);
    free(cx->ll_sym);
    free(cx->ml_sym);
    free(cx->of_sym);
    free(cx->ll_ex);
    free(cx->ml_ex);
    free(cx->of_ex);
    free(cx->grp);
    free(cx->fenc);
    memset(cx, 0, sizeof *cx);
}

parc_err parc_blk_dctx_init(parc_blk_dctx *dx, size_t max_block)
{
    if (max_block == 0 || max_block > PARC_LZ_MAX_BLOCK)
        return PARC_ERR_ARG;
    memset(dx, 0, sizeof *dx);
    size_t nseq_max = max_block / PARC_LZ_MIN_MATCH + 1;
    dx->max_block = max_block;
    dx->lit = malloc(max_block + PARC_WILDCOPY_SLACK);
    dx->sym = malloc(nseq_max);
    dx->ll = malloc(nseq_max * sizeof *dx->ll);
    dx->ml = malloc(nseq_max * sizeof *dx->ml);
    dx->dist = malloc(nseq_max * sizeof *dx->dist);
    dx->fdec = malloc(sizeof(parc_fdec));
    if (!dx->lit || !dx->sym || !dx->ll || !dx->ml || !dx->dist || !dx->fdec) {
        parc_blk_dctx_free(dx);
        return PARC_ERR_NOMEM;
    }
    return PARC_OK;
}

void parc_blk_dctx_free(parc_blk_dctx *dx)
{
    free(dx->lit);
    free(dx->sym);
    free(dx->ll);
    free(dx->ml);
    free(dx->dist);
    free(dx->fdec);
    memset(dx, 0, sizeof *dx);
}

/* ---- v0: Huffman over a flat token stream (FORMAT.md §2) ---- */

/* Encode the token stream toks[0..nt) into dst with capacity cap (bytes).
 * Returns PARC_BLK_PACKED with *comp_len on success, or PARC_BLK_STORED when
 * the encoding overflows cap (does not beat the current best / stored). */
static int encode_v0(parc_blk_cctx *cx, const uint8_t *src, uint32_t raw_len,
                     const parc_tok *toks, size_t nt, uint8_t *dst,
                     uint32_t cap, uint32_t *comp_len)
{
    (void)cx;
    (void)src;
    PARC_PROF_BEGIN(ee);
    uint32_t mfreq[MAIN_SYMS] = {0};
    uint32_t dfreq[DIST_SYMS] = {0};
    for (size_t t = 0; t < nt; ++t) {
        const parc_tok *tk = &toks[t];
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

    /* cap (<= raw_len - 1) makes "packed must beat the current best" automatic:
     * any overflow surfaces as PARC_ERR_LIMIT from the sticky writer */
    parc_bw w;
    parc_bw_init(&w, dst, cap);
    for (unsigned s = 0; s < MAIN_SYMS; ++s)
        parc_bw_put(&w, mlens[s], 4);
    for (unsigned s = 0; s < DIST_SYMS; ++s)
        parc_bw_put(&w, dlens[s], 4);
    for (size_t t = 0; t < nt; ++t) {
        const parc_tok *tk = &toks[t];
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
        PARC_PROF_END(ee, PARC_PROF_ENTROPY_ENCODE, raw_len);
        return PARC_BLK_STORED;
    }
    *comp_len = (uint32_t)bytes;
    PARC_PROF_END(ee, PARC_PROF_ENTROPY_ENCODE, raw_len);
    return PARC_BLK_PACKED;
}

static parc_err blk_decompress_v0(const uint8_t *comp, uint32_t comp_len,
                                  uint8_t *dst, uint32_t raw_len)
{
    parc_br r;
    parc_br_init(&r, comp, comp_len);
    PARC_PROF_BEGIN(ed);

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
        copy_match(dst, pos, dist, len);
        pos += len;
    }
    PARC_PROF_END(ed, PARC_PROF_ENTROPY_DECODE, raw_len);

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

/* ---- v1: FSE over a sequence model with repeat offsets (FORMAT.md §3) ---- */

/* Four-lane byte histogram. A single freq[syms[i]]++ table stalls on
 * store-to-load forwarding whenever nearby symbols repeat (very common in
 * literals); four independent tables summed at the end break that dependency
 * so the increments pipeline (the zstd HIST_count trick). Writes all 256
 * entries; symbols never reach an alphabet's unused high slots, which stay 0. */
static void hist_u8(uint32_t freq[256], const uint8_t *syms, size_t count)
{
    uint32_t c0[256] = {0}, c1[256] = {0}, c2[256] = {0}, c3[256] = {0};
    size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        c0[syms[i]]++;
        c1[syms[i + 1]]++;
        c2[syms[i + 2]]++;
        c3[syms[i + 3]]++;
    }
    for (; i < count; ++i)
        c0[syms[i]]++;
    for (unsigned s = 0; s < 256; ++s)
        freq[s] = c0[s] + c1[s] + c2[s] + c3[s];
}

/* Histogram, normalize, build the encode table, then write the table and the
 * FSE-coded symbols. count >= 1. */
static void emit_fse_stream(parc_bw *w, const uint8_t *syms, size_t count,
                            unsigned alpha, parc_fenc *fe, uint32_t *grp)
{
    uint32_t freq[LIT_SYMS];
    hist_u8(freq, syms, count);
    unsigned ms = 0;
    for (unsigned s = 0; s < alpha; ++s)
        if (freq[s])
            ms = s;
    unsigned tl = parc_fse_tablelog(count, ms);
    int16_t norm[LIT_SYMS];
    parc_fse_normalize(freq, ms, tl, norm);
    parc_fenc_build(fe, norm, ms, tl);
    parc_fse_write_table(w, norm, ms, tl);
    parc_fse_encode(w, fe, syms, count, grp);
}

/* v2 literal stream: canonical Huffman over the 256-byte literal alphabet.
 * Layout: 1 mode bit (0 = single stream; the 4-stream mode is reserved), then
 * 256 four-bit code lengths (FORMAT.md §2.3), then the Huffman-coded literals.
 * Huffman decodes several times faster than the tANS literal stream (one root
 * lookup, no ANS state chain) at neutral ratio, which is the v2 throughput
 * win. count >= 1 (position 0 is always a literal). */
static void emit_huff_literals(parc_bw *w, const uint8_t *lit, size_t count)
{
    uint32_t freq[256];
    hist_u8(freq, lit, count);
    uint8_t lens[256];
    parc_huff_lens(freq, 256, lens);
    parc_henc he;
    parc_henc_init(&he, lens, 256);
    /* count is redundant with raw_len - total_match; storing it lets the
     * decoder reject a mismatched raw_len deterministically (a wrong count
     * that reads a valid extra 0-code out of the zero padding would otherwise
     * slip the byte/padding check). 4 bytes/block is nil for the MiB blocks
     * this stream serves. */
    parc_bw_put(w, count, 32);
    parc_bw_put(w, 0, 1); /* mode: single stream */
    for (unsigned s = 0; s < 256; ++s)
        parc_bw_put(w, lens[s], 4);
    for (size_t i = 0; i < count; ++i)
        parc_henc_put(&he, w, lit[i]);
}

static parc_err read_huff_literals(parc_br *r, uint8_t *out, size_t count)
{
    PARC_PROF_BEGIN(ed);
    uint32_t stored = (uint32_t)parc_br_get(r, 32);
    if (r->failed)
        return PARC_ERR_TRUNCATED;
    if (stored != count)
        return PARC_ERR_CORRUPT; /* raw_len / total_match inconsistency */
    (void)parc_br_get(r, 1); /* mode bit (single stream) */
    uint8_t lens[256];
    for (unsigned s = 0; s < 256; ++s)
        lens[s] = (uint8_t)parc_br_get(r, 4);
    if (r->failed)
        return PARC_ERR_TRUNCATED;
    parc_hdec hd;
    if (parc_hdec_init(&hd, lens, 256) != PARC_OK || hd.nsyms == 0)
        return PARC_ERR_CORRUPT; /* count >= 1 needs a non-empty table */
    parc_err e = parc_hdec_decode(&hd, r, out, count);
    PARC_PROF_END(ed, PARC_PROF_ENTROPY_DECODE, 0);
    return e;
}

/* Emit the per-sequence extra-bits stream that trails an FSE symbol stream:
 * for each sequence i, the low bits of ex[i] whose width is the symbol's bucket
 * size — s - base - 1 bits when s > base, and none otherwise (repeat offsets and
 * empty buckets). The transcode sets ex[i] to exactly 0 whenever that width is
 * 0, and ex[i] < 2^width otherwise, so the payload is ORed in unconditionally:
 * no per-sequence branch to mispredict (the old three loops each branched on
 * `sym[i] >= 1`). The writer state is held in registers across the whole loop —
 * w's acc/nbits/pos otherwise round-trip to memory every put, because the byte
 * store may alias the local w — and the body replicates parc_bw_put's fast-path
 * 8-byte store, per-byte tail, and sticky-failed contract, so the wire output is
 * bit-identical. Mirrors parc_fse_encode's register-held emit; the same
 * discarded-on-overflow reasoning makes the post-failure divergence unobservable.
 * base is 0 for litLen/matchLen streams, NREP for the offset stream. */
static void emit_extra_bits(parc_bw *w, const uint8_t *sym, const uint32_t *ex,
                            uint32_t count, unsigned base)
{
    if (w->failed)
        return;
    uint8_t *dst = w->dst;
    size_t cap = w->cap;
    size_t pos = w->pos;
    uint64_t acc = w->acc;
    unsigned nbits = w->nbits;
    int failed = 0;
    for (uint32_t i = 0; i < count; ++i) {
        unsigned s = sym[i];
        unsigned nb = s > base ? s - base - 1u : 0u;
        acc |= (uint64_t)ex[i] << nbits; /* ex[i] == 0 when nb == 0 */
        nbits += nb;
        if (nbits >= 8) {
            if (pos + 8 <= cap) {
                unsigned whole = nbits >> 3;
                unsigned shift = whole * 8u;
                parc_bs_write_le64(dst + pos, acc);
                pos += whole;
                acc = shift >= 64 ? 0 : (acc >> shift);
                nbits -= shift;
            } else {
                while (nbits >= 8) {
                    uint8_t byte = (uint8_t)(acc & 0xFFu);
                    if (pos < cap) {
                        dst[pos] = byte;
                        pos++;
                    } else {
                        failed = 1;
                    }
                    acc >>= 8;
                    nbits -= 8;
                }
            }
        }
    }
    w->pos = pos;
    w->acc = acc;
    w->nbits = nbits;
    if (failed)
        w->failed = 1;
}

/* Top up `nbits` >= (need) bits in acc from src[pos..len) with one wide load
 * away from the tail, else byte-at-a-time (setting trunc on underrun). Mirrors
 * parc_br_fill exactly; the caller holds acc/nbits/pos/trunc as locals so the
 * register-held decode loops below never round-trip reader state to memory.
 * (need) must be <= 57, so one load always suffices. */
#define BE_REFILL(need)                                                        \
    do {                                                                       \
        if (nbits < (need)) {                                                  \
            if (pos + 8 <= len) {                                              \
                uint64_t word_ = parc_bs_read_le64(src + pos);                 \
                unsigned take_ = (64u - nbits) >> 3;                           \
                if (take_ < 8)                                                 \
                    word_ &= (UINT64_C(1) << (take_ * 8)) - 1;                 \
                acc |= word_ << nbits;                                         \
                pos += take_;                                                  \
                nbits += take_ * 8;                                            \
            } else {                                                           \
                while (nbits < (need) && pos < len) {                          \
                    acc |= (uint64_t)src[pos] << nbits;                        \
                    pos++;                                                     \
                    nbits += 8;                                                \
                }                                                              \
                if (nbits < (need))                                            \
                    trunc = 1;                                                 \
            }                                                                  \
        }                                                                      \
    } while (0)

/* Batched bucket-extra-bits decode. For each symbol sym[i], recover the value
 * that bucket_val() would (out[i] = add + bucket_val(sym[i] - sub)) but with a
 * register-held reader and one wide refill per several fields instead of a full
 * parc_br_get (avail multiply + sticky branch + refill) per field. This mirrors
 * parc_fse_decode's local-reader: acc/nbits/pos stay in registers across the
 * loop, truncation is tested once per refill, and the written-back reader state
 * (pos*8 - nbits tracks bits_consumed) is identical to what the per-get path
 * would leave, so downstream streams and the trailer check are unaffected.
 *
 * Symbols with sym[i] < sub are repeat-offset codes: they carry no extra bits
 * and get out[i] = 0 (the caller's recent-offset pass overwrites them). For the
 * litLen/matchLen streams sub == 0 and every symbol is a real bucket; for the
 * offset stream sub == NREP and add == 1 so out[i] is the new-offset distance.
 * On underrun the reader is marked failed (the caller returns CORRUPT). */
static inline __attribute__((always_inline)) void
decode_bucket_extras(parc_br *r, const uint8_t *sym, uint32_t *out,
                     uint32_t count, unsigned sub, uint32_t add)
{
    PARC_PROF_BEGIN(rc);
    const uint8_t *src = r->src;
    size_t len = r->len;
    size_t pos = r->pos;
    uint64_t acc = r->acc;
    unsigned nbits = r->nbits;
    int trunc = 0;

    for (uint32_t i = 0; i < count; ++i) {
        unsigned s = sym[i];
        if (s < sub) { /* repeat-offset code: no extra bits */
            out[i] = 0;
            continue;
        }
        unsigned b = s - sub;
        if (b == 0) {
            out[i] = add; /* bucket 0 -> value 0 */
            continue;
        }
        unsigned nb = b - 1; /* nb <= 23: fits one refill (< 57) */
        BE_REFILL(nb);
        if (trunc)
            break;
        uint32_t low = (uint32_t)(acc & ((UINT64_C(1) << nb) - 1));
        acc >>= nb;
        nbits -= nb;
        out[i] = add + (1u << nb) + low; /* (1 << (b-1)) + extra */
    }

    r->pos = pos;
    r->acc = acc;
    r->nbits = nbits;
    if (trunc)
        r->failed = 1;
    PARC_PROF_END(rc, PARC_PROF_RECONSTRUCT, 0);
}

/* Read an FSE table and decode `count` symbols into out. Returns PARC_OK or
 * PARC_ERR_CORRUPT/TRUNCATED. */
static parc_err read_fse_stream(parc_br *r, uint8_t *out, size_t count,
                                unsigned alpha, parc_fdec *fd)
{
    PARC_PROF_BEGIN(ed);
    int16_t norm[LIT_SYMS];
    unsigned ms = 0, tl = 0;
    parc_err err = parc_fse_read_table(r, norm, &ms, &tl, alpha - 1);
    if (err != PARC_OK)
        return err;
    err = parc_fdec_build(fd, norm, ms, tl);
    if (err != PARC_OK)
        return err;
    err = parc_fse_decode(r, fd, out, count);
    PARC_PROF_END(ed, PARC_PROF_ENTROPY_DECODE, 0);
    return err;
}

static int encode_v1(parc_blk_cctx *cx, const uint8_t *src, uint32_t raw_len,
                     const parc_tok *toks, size_t nt, uint8_t *dst,
                     uint32_t cap, uint32_t *comp_len)
{
    (void)src;

    /* Transcode tokens into a literal run and a sequence list, tracking the
     * recent-offset cache for repeat-offset codes. */
    PARC_PROF_BEGIN(tc);
    uint32_t recent[NREP] = {1, 2, 3};
    uint32_t nlit = 0, nseq = 0, pend = 0;
    for (size_t t = 0; t < nt; ++t) {
        const parc_tok *tk = &toks[t];
        if (tk->dist == 0) {
            cx->lit[nlit++] = (uint8_t)tk->len_or_lit;
            pend++;
            continue;
        }
        unsigned lb = bucket(pend);
        cx->ll_sym[nseq] = (uint8_t)lb;
        cx->ll_ex[nseq] = lb >= 1 ? bucket_rem(pend, lb) : 0;

        uint32_t mv = tk->len_or_lit - PARC_LZ_MIN_MATCH;
        unsigned mb = bucket(mv);
        cx->ml_sym[nseq] = (uint8_t)mb;
        cx->ml_ex[nseq] = mb >= 1 ? bucket_rem(mv, mb) : 0;

        uint32_t dist = tk->dist;
        int ri = dist == recent[0] ? 0
                 : dist == recent[1] ? 1
                 : dist == recent[2] ? 2
                                     : -1;
        if (ri >= 0) {
            cx->of_sym[nseq] = (uint8_t)ri;
            cx->of_ex[nseq] = 0;
            for (int k = ri; k > 0; --k)
                recent[k] = recent[k - 1];
            recent[0] = dist;
        } else {
            uint32_t dv = dist - 1;
            unsigned db = bucket(dv);
            cx->of_sym[nseq] = (uint8_t)(NREP + db);
            cx->of_ex[nseq] = db >= 1 ? bucket_rem(dv, db) : 0;
            recent[2] = recent[1];
            recent[1] = recent[0];
            recent[0] = dist;
        }
        nseq++;
        pend = 0;
    }
    PARC_PROF_END(tc, PARC_PROF_TRANSCODE, raw_len);

    PARC_PROF_BEGIN(ee);
    parc_bw w;
    parc_bw_init(&w, dst, cap);
    parc_bw_put(&w, nseq, 32);
    parc_fenc *fe = cx->fenc;
    if (nseq > 0) {
        emit_fse_stream(&w, cx->ll_sym, nseq, LL_SYMS, fe, cx->grp);
        emit_extra_bits(&w, cx->ll_sym, cx->ll_ex, nseq, 0);
        emit_fse_stream(&w, cx->ml_sym, nseq, ML_SYMS, fe, cx->grp);
        emit_extra_bits(&w, cx->ml_sym, cx->ml_ex, nseq, 0);
        emit_fse_stream(&w, cx->of_sym, nseq, OF_SYMS, fe, cx->grp);
        emit_extra_bits(&w, cx->of_sym, cx->of_ex, nseq, NREP);
    }
    /* literals always non-empty (position 0 is always a literal). v2 codes
     * them with fast Huffman; v1 with the tANS literal stream. */
    if (cx->version >= 2)
        emit_huff_literals(&w, cx->lit, nlit);
    else
        emit_fse_stream(&w, cx->lit, nlit, LIT_SYMS, fe, cx->grp);

    size_t bytes = 0;
    if (parc_bw_finish(&w, &bytes) != PARC_OK) {
        *comp_len = raw_len;
        PARC_PROF_END(ee, PARC_PROF_ENTROPY_ENCODE, raw_len);
        return PARC_BLK_STORED;
    }
    *comp_len = (uint32_t)bytes;
    PARC_PROF_END(ee, PARC_PROF_ENTROPY_ENCODE, raw_len);
    return PARC_BLK_PACKED;
}

static parc_err blk_decompress_v1(parc_blk_dctx *dx, const uint8_t *comp,
                                  uint32_t comp_len, uint8_t *dst,
                                  uint32_t raw_len, int huff_lit)
{
    parc_br r;
    parc_br_init(&r, comp, comp_len);

    uint32_t nseq = (uint32_t)parc_br_get(&r, 32);
    if (r.failed || nseq > raw_len / PARC_LZ_MIN_MATCH)
        return PARC_ERR_CORRUPT;

    parc_fdec *fd = dx->fdec;
    uint64_t total_match = 0;
    if (nseq > 0) {
        if (read_fse_stream(&r, dx->sym, nseq, LL_SYMS, fd) != PARC_OK)
            return PARC_ERR_CORRUPT;
        decode_bucket_extras(&r, dx->sym, dx->ll, nseq, 0, 0);
        if (read_fse_stream(&r, dx->sym, nseq, ML_SYMS, fd) != PARC_OK)
            return PARC_ERR_CORRUPT;
        decode_bucket_extras(&r, dx->sym, dx->ml, nseq, 0, PARC_LZ_MIN_MATCH);
        if (r.failed)
            return PARC_ERR_CORRUPT;
        for (uint32_t i = 0; i < nseq; ++i)
            total_match += dx->ml[i];
        if (total_match > raw_len)
            return PARC_ERR_CORRUPT;
        if (read_fse_stream(&r, dx->sym, nseq, OF_SYMS, fd) != PARC_OK)
            return PARC_ERR_CORRUPT;
        /* Offset decode: read each new-offset's extra bits and resolve the
         * recent-offset cache in one pass, with a register-held reader (the
         * parc_fse_decode pattern) so the pass touches sym[]/dist[] once. The
         * recent slots are scalar (r0 newest) so the repeat-code shuffle is a
         * few moves, not an indexed inner loop. Repeat codes (s < NREP) carry
         * no extra bits. Bit-exact with the per-get path: same whole bytes in
         * the same order, and pos*8 - nbits tracks bits_consumed identically. */
        {
            PARC_PROF_BEGIN(rc);
            const uint8_t *src = r.src;
            size_t len = r.len, pos = r.pos;
            uint64_t acc = r.acc;
            unsigned nbits = r.nbits;
            int trunc = 0;
            uint32_t r0 = 1, r1 = 2, r2 = 3;
            for (uint32_t i = 0; i < nseq; ++i) {
                unsigned s = dx->sym[i];
                uint32_t dist;
                if (s < NREP) {
                    if (s == 0) {
                        dist = r0;
                    } else if (s == 1) {
                        dist = r1;
                        r1 = r0;
                    } else {
                        dist = r2;
                        r2 = r1;
                        r1 = r0;
                    }
                } else {
                    unsigned b = s - NREP;
                    uint32_t dv = 0;
                    if (b != 0) {
                        unsigned nb = b - 1; /* nb <= 23: one refill suffices */
                        BE_REFILL(nb);
                        if (trunc)
                            break;
                        dv = (1u << nb) +
                             (uint32_t)(acc & ((UINT64_C(1) << nb) - 1));
                        acc >>= nb;
                        nbits -= nb;
                    }
                    dist = dv + 1;
                    r2 = r1;
                    r1 = r0;
                }
                r0 = dist;
                dx->dist[i] = dist;
            }
            r.pos = pos;
            r.acc = acc;
            r.nbits = nbits;
            if (trunc)
                r.failed = 1;
            PARC_PROF_END(rc, PARC_PROF_RECONSTRUCT, 0);
        }
        if (r.failed)
            return PARC_ERR_CORRUPT;
    }

    uint32_t nlit = (uint32_t)(raw_len - total_match);
    if (nlit == 0) /* position 0 is always a literal */
        return PARC_ERR_CORRUPT;
    parc_err lerr = huff_lit ? read_huff_literals(&r, dx->lit, nlit)
                             : read_fse_stream(&r, dx->lit, nlit, LIT_SYMS, fd);
    if (lerr != PARC_OK)
        return PARC_ERR_CORRUPT;

    PARC_PROF_BEGIN(rc);
    uint32_t pos = 0, li = 0;
    for (uint32_t i = 0; i < nseq; ++i) {
        uint32_t ll = dx->ll[i];
        if (ll > nlit - li)
            return PARC_ERR_CORRUPT;
        wild_copy(dst + pos, dx->lit + li, ll); /* dst/lit carry slack */
        pos += ll;
        li += ll;
        uint32_t dist = dx->dist[i], m = dx->ml[i];
        if (dist > pos || m > raw_len - pos)
            return PARC_ERR_CORRUPT;
        copy_match(dst, pos, dist, m);
        pos += m;
    }
    uint32_t tail = nlit - li;
    memcpy(dst + pos, dx->lit + li, tail);
    pos += tail;
    PARC_PROF_END(rc, PARC_PROF_RECONSTRUCT, raw_len);
    if (pos != raw_len)
        return PARC_ERR_CORRUPT;

    /* comp_len must be minimal and padding bits zero (§2/§3) */
    uint64_t used = parc_br_bits_consumed(&r);
    if ((used + 7) / 8 != comp_len)
        return PARC_ERR_CORRUPT;
    unsigned pad = (unsigned)((uint64_t)comp_len * 8 - used);
    if (pad != 0 && parc_br_get(&r, pad) != 0)
        return PARC_ERR_CORRUPT;
    return PARC_OK;
}

/* ---- dispatch ---- */

/* Encode toks[0..nt) in the context's wire version into dst with capacity cap. */
static int encode_block(parc_blk_cctx *cx, const uint8_t *src, uint32_t raw_len,
                        const parc_tok *toks, size_t nt, uint8_t *dst,
                        uint32_t cap, uint32_t *comp_len)
{
    return cx->version >= 1
               ? encode_v1(cx, src, raw_len, toks, nt, dst, cap, comp_len)
               : encode_v0(cx, src, raw_len, toks, nt, dst, cap, comp_len);
}

int parc_blk_compress(parc_blk_cctx *cx, const uint8_t *src, uint32_t raw_len,
                      uint8_t *dst, uint32_t *comp_len)
{
    assert(raw_len >= 1 && raw_len <= cx->max_block);

    /* Non-optimal levels: one matcher, one encode; capacity raw_len - 1 makes
     * "packed must beat stored" automatic. */
    if (!cx->cfg.optimal) {
        PARC_PROF_BEGIN(m);
        size_t nt = cx->cfg.max_chain
                        ? parc_lz_chain(src, raw_len, cx->toks, cx->htab,
                                        cx->prev, cx->cfg)
                        : parc_lz_greedy(src, raw_len, cx->toks, cx->htab);
        PARC_PROF_END(m, PARC_PROF_LZ_MATCH, raw_len);
        return encode_block(cx, src, raw_len, cx->toks, nt, dst, raw_len - 1,
                            comp_len);
    }

    /* Optimal levels: the cost-based parse minimizes an estimated bit cost,
     * which can misjudge blocks the real entropy stage prices differently (it
     * loses on some small/binary blocks). So encode both a lazy parse and the
     * optimal parse and keep the smaller — the optimal tier is then never worse
     * than the lazy one. The lazy candidate uses the strongest non-optimal
     * config (level 7) so the optimal tiers are also never worse than level 7,
     * regardless of how the optimal search is tuned. It lands in dst; the
     * optimal candidate encodes into cx->alt capped one byte under the lazy
     * size, so it is kept only when it strictly wins. */
    parc_lz_cfg lazy = parc_lz_cfg_for_level(PARC_LZ_LEVEL_MAX - 2);
    PARC_PROF_BEGIN(ml);
    size_t nt_lazy = parc_lz_chain(src, raw_len, cx->toks, cx->htab, cx->prev,
                                   lazy);
    PARC_PROF_END(ml, PARC_PROF_LZ_MATCH, raw_len);
    uint32_t cl_lazy = 0;
    int r_lazy = encode_block(cx, src, raw_len, cx->toks, nt_lazy, dst,
                              raw_len - 1, &cl_lazy);

    PARC_PROF_BEGIN(mo);
    size_t nt_opt = parc_lz_optimal(src, raw_len, cx->toks, cx->htab, cx->prev,
                                    cx->cfg, cx->opt_price, cx->opt_len,
                                    cx->opt_dist, cx->opt_rep);
    PARC_PROF_END(mo, PARC_PROF_LZ_MATCH, 0);
    uint32_t opt_cap = r_lazy == PARC_BLK_PACKED ? cl_lazy - 1 : raw_len - 1;
    uint32_t cl_opt = 0;
    int r_opt = encode_block(cx, src, raw_len, cx->toks, nt_opt, cx->alt,
                             opt_cap, &cl_opt);

    if (r_opt == PARC_BLK_PACKED) {
        memcpy(dst, cx->alt, cl_opt);
        *comp_len = cl_opt;
        return PARC_BLK_PACKED;
    }
    *comp_len = cl_lazy;
    return r_lazy;
}

parc_err parc_blk_decompress(parc_blk_dctx *dx, const uint8_t *comp,
                             uint32_t comp_len, uint8_t *dst, uint32_t raw_len,
                             unsigned version)
{
    if (version == 1 || version == 2) {
        assert(dx != NULL && raw_len <= dx->max_block);
        return blk_decompress_v1(dx, comp, comp_len, dst, raw_len, version == 2);
    }
    return blk_decompress_v0(comp, comp_len, dst, raw_len);
}
