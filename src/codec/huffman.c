#include "codec/huffman.h"

#include <assert.h>
#include <string.h>

/* Kraft sum bookkeeping is done in units of 2^-15: a code of length L
 * contributes 1 << (15 - L) units, and a complete code sums to exactly
 * KRAFT_ONE. */
#define KRAFT_ONE (1u << PARC_HUFF_MAX_LEN)

/* Decode-table cell = (symbol << 4) | len-nibble. Direct codes carry their
 * true length (1..PARC_HDEC_ROOT_BITS) in the nibble; two nibble values that
 * a direct length can never take are reserved: */
#define HDEC_INVALID_NIB 0u  /* no code with this prefix -> reject */
#define HDEC_LONG_NIB 15u    /* code longer than root_bits -> bit-serial walk */
#define HDEC_INVALID ((uint16_t)HDEC_INVALID_NIB)
#define HDEC_LONG ((uint16_t)HDEC_LONG_NIB)

/* ---- code length computation ---- */

/* Build an exact (unlimited-depth) Huffman tree over the nused sorted leaf
 * weights and return each leaf's depth in ldepth. Two-queue method: leaves
 * ascending in w, internal nodes are created in nondecreasing weight order,
 * so both queues stay sorted and each merge takes O(1). */
static void huff_tree_depths(const uint32_t *w, unsigned nused,
                             uint16_t *ldepth)
{
    /* parent index of each leaf / internal node, in the internal array */
    uint16_t lpar[PARC_HUFF_MAX_SYMS];
    uint16_t ipar[PARC_HUFF_MAX_SYMS];
    uint32_t iw[PARC_HUFF_MAX_SYMS];
    uint16_t idepth[PARC_HUFF_MAX_SYMS];
    unsigned li = 0, ii = 0, made = 0;

    assert(nused >= 2);
    for (unsigned k = 0; k + 1 < nused; ++k) {
        uint32_t sum = 0;
        for (int half = 0; half < 2; ++half) {
            /* take the smaller front of the two queues; ties prefer leaves
             * (slightly shallower trees for equal weights) */
            if (li < nused && (ii >= made || w[li] <= iw[ii])) {
                sum += w[li];
                lpar[li++] = (uint16_t)made;
            } else {
                sum += iw[ii];
                ipar[ii++] = (uint16_t)made;
            }
        }
        iw[made++] = sum;
    }
    /* root is the last internal node; parents always have higher index */
    idepth[made - 1] = 0;
    for (unsigned k = made - 1; k-- > 0;)
        idepth[k] = (uint16_t)(idepth[ipar[k]] + 1);
    for (unsigned i = 0; i < nused; ++i)
        ldepth[i] = (uint16_t)(idepth[lpar[i]] + 1);
}

void parc_huff_lens(const uint32_t *freq, unsigned n, uint8_t *lens)
{
    /* used symbols sorted by (freq asc, symbol asc); insertion sort is fine
     * for n <= 282 */
    uint16_t order[PARC_HUFF_MAX_SYMS];
    uint32_t w[PARC_HUFF_MAX_SYMS];
    unsigned nused = 0;

    assert(n >= 1 && n <= PARC_HUFF_MAX_SYMS);
    memset(lens, 0, n);
    for (unsigned s = 0; s < n; ++s) {
        if (freq[s] == 0)
            continue;
        unsigned i = nused++;
        while (i > 0 && w[i - 1] > freq[s]) {
            w[i] = w[i - 1];
            order[i] = order[i - 1];
            --i;
        }
        w[i] = freq[s];
        order[i] = (uint16_t)s;
    }

    if (nused == 0)
        return; /* empty table */
    if (nused == 1) {
        lens[order[0]] = 1; /* degenerate table */
        return;
    }

    uint16_t depth[PARC_HUFF_MAX_SYMS];
    huff_tree_depths(w, nused, depth);

    /* clamp to the 15-bit limit, then repair the Kraft sum to exactly
     * KRAFT_ONE (clamping only ever adds excess, so kraft >= KRAFT_ONE) */
    uint32_t kraft = 0;
    for (unsigned i = 0; i < nused; ++i) {
        if (depth[i] > PARC_HUFF_MAX_LEN)
            depth[i] = PARC_HUFF_MAX_LEN;
        kraft += 1u << (PARC_HUFF_MAX_LEN - depth[i]);
    }
    /* shed excess: lengthen the rarest symbols (lowest index in sorted
     * order) that are not yet at the limit */
    while (kraft > KRAFT_ONE) {
        for (unsigned i = 0; i < nused && kraft > KRAFT_ONE; ++i) {
            while (depth[i] < PARC_HUFF_MAX_LEN && kraft > KRAFT_ONE) {
                kraft -= 1u << (PARC_HUFF_MAX_LEN - depth[i] - 1);
                depth[i]++;
            }
        }
    }
    /* fill any deficit: shorten a deepest symbol, preferring the most
     * frequent one (highest index). The deficit is always divisible by the
     * deepest symbol's contribution, so this never overshoots. */
    while (kraft < KRAFT_ONE) {
        unsigned best = 0;
        uint16_t maxd = 0;
        for (unsigned i = 0; i < nused; ++i) {
            if (depth[i] >= maxd) {
                maxd = depth[i];
                best = i;
            }
        }
        assert(maxd >= 2);
        kraft += 1u << (PARC_HUFF_MAX_LEN - maxd);
        depth[best]--;
    }

    for (unsigned i = 0; i < nused; ++i)
        lens[order[i]] = (uint8_t)depth[i];
}

/* ---- canonical code assignment ---- */

/* first canonical code of each length, per the DEFLATE recurrence */
static void canonical_first(const uint16_t *count, uint16_t *first)
{
    uint32_t code = 0;
    first[0] = 0;
    for (unsigned l = 1; l <= PARC_HUFF_MAX_LEN; ++l) {
        code = (code + count[l - 1]) << 1;
        first[l] = (uint16_t)code;
    }
}

static uint16_t bit_reverse(uint32_t v, unsigned nbits)
{
    uint32_t r = 0;
    for (unsigned i = 0; i < nbits; ++i) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return (uint16_t)r;
}

void parc_henc_init(parc_henc *e, const uint8_t *lens, unsigned n)
{
    uint16_t count[PARC_HUFF_MAX_LEN + 1] = {0};
    uint16_t next[PARC_HUFF_MAX_LEN + 1];

    assert(n <= PARC_HUFF_MAX_SYMS);
    for (unsigned s = 0; s < n; ++s) {
        assert(lens[s] <= PARC_HUFF_MAX_LEN);
        count[lens[s]]++;
    }
    count[0] = 0;
    canonical_first(count, next);
    for (unsigned s = 0; s < n; ++s) {
        e->len[s] = lens[s];
        e->code[s] = lens[s] ? bit_reverse(next[lens[s]]++, lens[s]) : 0;
    }
}

parc_err parc_hdec_init(parc_hdec *d, const uint8_t *lens, unsigned n)
{
    assert(n <= PARC_HUFF_MAX_SYMS);
    memset(d->count, 0, sizeof d->count);
    d->nsyms = 0;
    d->maxlen = 0;
    d->root_bits = 0;

    uint32_t kraft = 0;
    uint8_t only_len = 0;
    for (unsigned s = 0; s < n; ++s) {
        if (lens[s] == 0)
            continue;
        if (lens[s] > PARC_HUFF_MAX_LEN)
            return PARC_ERR_CORRUPT;
        d->count[lens[s]]++;
        d->nsyms++;
        only_len = lens[s];
        kraft += 1u << (PARC_HUFF_MAX_LEN - lens[s]);
    }
    if (d->nsyms == 0)
        return PARC_OK; /* empty table (distance alphabet only) */
    if (d->nsyms == 1) {
        if (only_len != 1)
            return PARC_ERR_CORRUPT; /* degenerate table must use length 1 */
    } else if (kraft != KRAFT_ONE) {
        return PARC_ERR_CORRUPT; /* over- or under-subscribed */
    }

    canonical_first(d->count, d->first);
    uint16_t at = 0;
    uint16_t pos[PARC_HUFF_MAX_LEN + 1];
    for (unsigned l = 1; l <= PARC_HUFF_MAX_LEN; ++l) {
        d->offset[l] = at;
        pos[l] = at;
        at = (uint16_t)(at + d->count[l]);
    }
    d->offset[0] = 0;
    for (unsigned s = 0; s < n; ++s)
        if (lens[s])
            d->syms[pos[lens[s]]++] = (uint16_t)s;

    unsigned maxlen = PARC_HUFF_MAX_LEN;
    while (maxlen > 0 && d->count[maxlen] == 0)
        --maxlen;
    d->maxlen = (uint8_t)maxlen;
    unsigned rb = maxlen < PARC_HDEC_ROOT_BITS ? maxlen : PARC_HDEC_ROOT_BITS;
    d->root_bits = (uint8_t)rb;

    /* Build the direct table. Cells default to HDEC_INVALID (no code). Each
     * present code fills either every cell sharing its bit-reversed value in
     * the low `len` bits (len <= rb), or the single root cell of its prefix,
     * marked HDEC_LONG for the bit-serial fallback (len > rb). A prefix-free
     * code guarantees these two never target the same cell. */
    for (uint32_t i = 0; i < (1u << rb); ++i)
        d->tbl[i] = HDEC_INVALID;
    for (unsigned len = 1; len <= maxlen; ++len) {
        uint16_t code = d->first[len];
        for (unsigned i = 0; i < d->count[len]; ++i, ++code) {
            uint16_t sym = d->syms[d->offset[len] + i];
            if (len <= rb) {
                uint16_t rev = bit_reverse(code, len);
                uint16_t entry = (uint16_t)(((unsigned)sym << 4) | len);
                for (uint32_t k = 0; k < (1u << (rb - len)); ++k)
                    d->tbl[rev | (k << len)] = entry;
            } else {
                uint16_t top = (uint16_t)(code >> (len - rb));
                d->tbl[bit_reverse(top, rb)] = HDEC_LONG;
            }
        }
    }
    return PARC_OK;
}

/* Bit-serial resolve for codes longer than root_bits (the reference walk,
 * matching the canonical assignment). Returns the symbol or -1. */
static int hdec_long(const parc_hdec *d, parc_br *r)
{
    uint32_t code = 0;
    for (unsigned l = 1; l <= d->maxlen; ++l) {
        code = (code << 1) | (uint32_t)parc_br_get(r, 1);
        if (d->count[l] && code >= d->first[l] &&
            code - d->first[l] < d->count[l])
            return d->syms[d->offset[l] + (code - d->first[l])];
    }
    return -1;
}

int parc_hdec_get(const parc_hdec *d, parc_br *r)
{
    uint16_t e = d->tbl[parc_br_peek(r, d->root_bits)];
    unsigned len = e & 0xF;
    if (len == HDEC_INVALID_NIB)
        return -1; /* no code with this prefix */
    if (len == HDEC_LONG_NIB)
        return hdec_long(d, r);
    /* consume the code's bits; get fails (sets r->failed) if they run past
     * the end, which the caller checks. */
    (void)parc_br_get(r, len);
    return e >> 4;
}

parc_err parc_hdec_decode(const parc_hdec *d, parc_br *r, uint8_t *out,
                          size_t count)
{
    if (count == 0)
        return PARC_OK;
    if (r->failed)
        return PARC_ERR_TRUNCATED;

    /* Register-held reader (mirrors parc_fse_decode): hold acc/nbits/pos in
     * locals, top up from whole bytes, and resolve each symbol with one root
     * lookup when a full root_bits window is buffered. The rare cases — a code
     * longer than root_bits, an invalid prefix, or the last few symbols where
     * fewer than root_bits remain — sync back to r and defer to parc_hdec_get,
     * which handles partial availability and the bit-serial walk. */
    const uint8_t *src = r->src;
    size_t len = r->len;
    size_t pos = r->pos;
    uint64_t acc = r->acc;
    unsigned nbits = r->nbits;
    unsigned rb = d->root_bits;

    for (size_t i = 0; i < count; ++i) {
        if (nbits < rb) {
            if (pos + 8 <= len) { /* one wide little-endian load tops to >=57 */
                uint64_t word = parc_bs_read_le64(src + pos);
                unsigned take = (64u - nbits) >> 3;
                if (take < 8)
                    word &= (UINT64_C(1) << (take * 8)) - 1;
                acc |= word << nbits;
                pos += take;
                nbits += take * 8;
            } else {
                while (nbits <= 56 && pos < len) {
                    acc |= (uint64_t)src[pos] << nbits;
                    pos++;
                    nbits += 8;
                }
            }
        }
        if (nbits >= rb) {
            uint16_t e = d->tbl[acc & ((1u << rb) - 1u)];
            unsigned l = e & 0xF;
            if (l != HDEC_INVALID_NIB && l != HDEC_LONG_NIB) {
                acc >>= l;
                nbits -= l;
                out[i] = (uint8_t)(e >> 4);
                continue;
            }
        }
        /* rare / tail: defer to the safe per-symbol decoder */
        r->pos = pos;
        r->acc = acc;
        r->nbits = nbits;
        int s = parc_hdec_get(d, r);
        if (s < 0 || r->failed) {
            if (r->failed)
                return PARC_ERR_TRUNCATED;
            r->failed = 1;
            return PARC_ERR_CORRUPT;
        }
        out[i] = (uint8_t)s;
        pos = r->pos;
        acc = r->acc;
        nbits = r->nbits;
    }

    r->pos = pos;
    r->acc = acc;
    r->nbits = nbits;
    return PARC_OK;
}
