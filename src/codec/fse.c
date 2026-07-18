#include "codec/fse.h"

#include <assert.h>
#include <string.h>

/* highbit(x) = floor(log2(x)) for x >= 1. */
static unsigned highbit(uint32_t x)
{
    assert(x != 0);
    return 31u - (unsigned)__builtin_clz(x);
}

unsigned parc_fse_tablelog(size_t count, unsigned max_symbol)
{
    assert(count >= 1);
    unsigned tl = PARC_FSE_DEFAULT_TABLELOG;

    /* Shrink for small inputs: a table much larger than the message wastes
     * header bits without buying precision. Keep 2^tl within ~2x of count. */
    while (tl > PARC_FSE_MIN_TABLELOG && ((size_t)1u << (tl - 1)) > count)
        --tl;

    /* Grow if needed so every symbol in the alphabet can get a count >= 1:
     * the table must be at least as large as the alphabet. */
    unsigned min_log = highbit(max_symbol | 1u) + 1u;
    if (min_log < PARC_FSE_MIN_TABLELOG)
        min_log = PARC_FSE_MIN_TABLELOG;
    if (tl < min_log)
        tl = min_log;
    if (tl > PARC_FSE_MAX_TABLELOG)
        tl = PARC_FSE_MAX_TABLELOG;
    return tl;
}

parc_err parc_fse_normalize(const uint32_t *freq, unsigned max_symbol,
                            unsigned table_log, int16_t *norm)
{
    uint32_t table_size = 1u << table_log;
    uint64_t total = 0;
    unsigned used = 0;

    for (unsigned s = 0; s <= max_symbol; ++s) {
        norm[s] = 0;
        if (freq[s]) {
            total += freq[s];
            ++used;
        }
    }
    assert(total > 0);
    if (used > table_size)
        return PARC_ERR_LIMIT;

    /* Every present symbol gets a base count of 1; the surplus is handed out
     * in proportion to frequency (largest-remainder rounding), so the counts
     * sum to table_size exactly. */
    uint32_t surplus = table_size - used;
    uint64_t rem[PARC_FSE_MAX_SYMS];
    uint64_t handed = 0;
    for (unsigned s = 0; s <= max_symbol; ++s) {
        rem[s] = 0;
        if (!freq[s])
            continue;
        uint64_t scaled = (uint64_t)freq[s] * surplus;
        norm[s] = (int16_t)(1u + (uint32_t)(scaled / total));
        rem[s] = scaled % total;
        handed += scaled / total;
    }

    /* Distribute the rounding leftover to the largest fractional remainders.
     * The fractional parts sum to `leftover`, so at least that many symbols
     * have a nonzero remainder — each is picked at most once. */
    uint32_t leftover = surplus - (uint32_t)handed;
    while (leftover) {
        unsigned best = max_symbol + 1;
        for (unsigned s = 0; s <= max_symbol; ++s) {
            if (rem[s] == 0)
                continue;
            /* tie-break toward the more frequent symbol for determinism */
            if (best > max_symbol || rem[s] > rem[best] ||
                (rem[s] == rem[best] && freq[s] > freq[best]))
                best = s;
        }
        assert(best <= max_symbol);
        norm[best] = (int16_t)(norm[best] + 1);
        rem[best] = 0; /* consumed; never repicked */
        --leftover;
    }
    return PARC_OK;
}

/* ---- table build ---- */

/* Scatter the alphabet across the state table: symbol s occupies norm[s]
 * slots, visited by a fixed coprime stride so occurrences are spread out.
 * Both encode and decode build use the identical order, which is what makes
 * them dual. Fills sym[0..table_size). */
static void fse_spread(const int16_t *norm, unsigned max_symbol,
                       unsigned table_log, uint16_t *sym)
{
    uint32_t table_size = 1u << table_log;
    uint32_t mask = table_size - 1u;
    uint32_t step = (table_size >> 1) + (table_size >> 3) + 3u;
    uint32_t pos = 0;

    for (unsigned s = 0; s <= max_symbol; ++s) {
        for (int16_t k = 0; k < norm[s]; ++k) {
            sym[pos] = (uint16_t)s;
            pos = (pos + step) & mask;
        }
    }
    /* step is coprime to the power-of-two table_size, so pos returns to 0
     * only after visiting every slot exactly once. */
    assert(pos == 0);
}

void parc_fenc_build(parc_fenc *e, const int16_t *norm, unsigned max_symbol,
                     unsigned table_log)
{
    uint32_t table_size = 1u << table_log;
    uint16_t sym[1u << PARC_FSE_MAX_TABLELOG];
    uint32_t cumul[PARC_FSE_MAX_SYMS + 1];

    e->table_log = table_log;
    fse_spread(norm, max_symbol, table_log, sym);

    cumul[0] = 0;
    for (unsigned s = 0; s <= max_symbol; ++s) {
        uint32_t ns = (uint32_t)norm[s];
        cumul[s + 1] = cumul[s] + ns;
    }

    /* state_table maps each symbol's cumulative slots to encoder states. */
    for (uint32_t u = 0; u < table_size; ++u) {
        uint16_t s = sym[u];
        e->state_table[cumul[s]++] = (uint16_t)(table_size + u);
    }

    /* Per-symbol transform (deltaNbBits/deltaFindState), reset cumul first. */
    cumul[0] = 0;
    for (unsigned s = 0; s <= max_symbol; ++s) {
        uint32_t ns = (uint32_t)norm[s];
        cumul[s + 1] = cumul[s] + ns;
    }

    uint32_t total = 0;
    for (unsigned s = 0; s <= max_symbol; ++s) {
        int16_t n = norm[s];
        if (n == 0) {
            /* unused symbol: a value that is never consulted */
            e->delta_nbits[s] = ((table_log + 1u) << 16) - table_size;
            e->delta_find[s] = 0;
        } else if (n == 1) {
            e->delta_nbits[s] = (table_log << 16) - table_size;
            e->delta_find[s] = (int32_t)total - 1;
            total += 1;
        } else {
            unsigned max_bits = table_log - highbit((uint32_t)(n - 1));
            uint32_t min_state = (uint32_t)n << max_bits;
            e->delta_nbits[s] = (max_bits << 16) - min_state;
            e->delta_find[s] = (int32_t)total - n;
            uint32_t un = (uint32_t)n;
            total += un;
        }
    }
}

parc_err parc_fdec_build(parc_fdec *d, const int16_t *norm, unsigned max_symbol,
                         unsigned table_log)
{
    uint32_t table_size = 1u << table_log;
    uint16_t sym[1u << PARC_FSE_MAX_TABLELOG];
    uint16_t next[PARC_FSE_MAX_SYMS];
    uint64_t sum = 0;

    for (unsigned s = 0; s <= max_symbol; ++s) {
        if (norm[s] < 0 || (uint32_t)norm[s] > table_size)
            return PARC_ERR_CORRUPT;
        uint32_t ns = (uint32_t)norm[s];
        sum += ns;
    }
    if (sum != table_size)
        return PARC_ERR_CORRUPT;

    d->table_log = table_log;
    fse_spread(norm, max_symbol, table_log, sym);

    for (unsigned s = 0; s <= max_symbol; ++s)
        next[s] = (uint16_t)norm[s];

    for (uint32_t u = 0; u < table_size; ++u) {
        uint16_t s = sym[u];
        uint32_t nstate = next[s]++;
        unsigned nb = table_log - highbit(nstate);
        d->symbol[u] = (uint8_t)s;
        d->nbits[u] = (uint8_t)nb;
        d->new_state[u] = (uint16_t)((nstate << nb) - table_size);
    }
    return PARC_OK;
}

/* ---- table (de)serialization ---- */

/* Counts are bucket-coded (§3.1): a 4-bit bucket b = bit_length(count) then
 * b-1 mantissa bits, so the small counts of a normalized table cost ~4-7 bits
 * each instead of a fixed table_log+1. b never exceeds table_log+1 <= 13. */
void parc_fse_write_table(parc_bw *w, const int16_t *norm, unsigned max_symbol,
                          unsigned table_log)
{
    parc_bw_put(w, table_log, 4);
    parc_bw_put(w, max_symbol, 8);
    for (unsigned s = 0; s <= max_symbol; ++s) {
        uint32_t c = (uint32_t)norm[s];
        unsigned b = c == 0 ? 0 : 32u - (unsigned)__builtin_clz(c);
        parc_bw_put(w, b, 4);
        if (b >= 1)
            parc_bw_put(w, c - (1u << (b - 1)), b - 1);
    }
}

parc_err parc_fse_read_table(parc_br *r, int16_t *norm, unsigned *max_symbol,
                             unsigned *table_log, unsigned limit)
{
    assert(limit < PARC_FSE_MAX_SYMS);
    unsigned tl = (unsigned)parc_br_get(r, 4);
    if (tl < PARC_FSE_MIN_TABLELOG || tl > PARC_FSE_MAX_TABLELOG)
        return PARC_ERR_CORRUPT;
    unsigned ms = (unsigned)parc_br_get(r, 8);
    if (ms > limit)
        return PARC_ERR_CORRUPT;

    uint32_t table_size = 1u << tl;
    uint64_t sum = 0;
    for (unsigned s = 0; s <= ms; ++s) {
        unsigned b = (unsigned)parc_br_get(r, 4);
        if (b > tl + 1u)
            return PARC_ERR_CORRUPT; /* count would exceed table_size */
        uint32_t c = b == 0 ? 0
                            : (1u << (b - 1)) + (uint32_t)parc_br_get(r, b - 1);
        norm[s] = (int16_t)c;
        sum += c;
    }
    for (unsigned s = ms + 1; s <= limit; ++s)
        norm[s] = 0;
    if (parc_br_err(r) != PARC_OK)
        return PARC_ERR_TRUNCATED;
    if (sum != table_size || norm[ms] == 0)
        return PARC_ERR_CORRUPT; /* bad sum, or max_symbol not actually present */

    *max_symbol = ms;
    *table_log = tl;
    return PARC_OK;
}

/* ---- symbol coding ---- */

parc_err parc_fse_encode(parc_bw *w, const parc_fenc *e, const uint8_t *syms,
                         size_t count, uint32_t *grp)
{
    if (count == 0)
        return PARC_OK;

    unsigned table_log = e->table_log;
    uint32_t mask = (1u << table_log) - 1u;

    /* Initialize the state from the last symbol (emits no bits). */
    uint8_t last = syms[count - 1];
    uint32_t nb0 = (e->delta_nbits[last] + (1u << 15)) >> 16;
    uint32_t state = (nb0 << 16) - e->delta_nbits[last];
    state = e->state_table[(state >> nb0) + (uint32_t)e->delta_find[last]];

    /* Encode the remaining symbols in reverse, recording one group each. */
    size_t ng = 0;
    for (size_t i = count - 1; i-- > 0;) {
        uint8_t s = syms[i];
        uint32_t nb = (state + e->delta_nbits[s]) >> 16;
        uint32_t val = state & ((1u << nb) - 1u);
        grp[ng++] = (nb << 16) | val;
        state = e->state_table[(state >> nb) + (uint32_t)e->delta_find[s]];
    }
    /* Flush the final state (table_log bits) as the last group. */
    grp[ng++] = (table_log << 16) | (state & mask);

    /* Emit groups in reverse (decode) order: final state first, then the
     * transitions in forward order, so a forward reader recovers symbols in
     * order. Drain into locals held in registers across the whole loop rather
     * than a parc_bw_put call per group: w is a pointer, so each call reloads
     * w->acc/nbits/pos from memory (the compiler cannot prove w->dst and grp
     * don't alias) — the writer state round-trips to memory every symbol.
     * Mirrors the local-reader in parc_fse_decode. The body replicates
     * parc_bw_put exactly (same 8-byte fast-path store, same per-byte tail,
     * same sticky-failed contract), so the wire output is bit-identical; when
     * capacity is exceeded the block is stored raw and this bitstream is
     * discarded, so letting acc/nbits keep advancing past the first overflow
     * (instead of freezing as parc_bw_put would) is not observable. */
    if (!w->failed) {
        uint8_t *dst = w->dst;
        size_t cap = w->cap;
        size_t pos = w->pos;
        uint64_t acc = w->acc;
        unsigned nbits = w->nbits;
        int failed = 0;
        for (size_t j = ng; j-- > 0;) {
            unsigned n = grp[j] >> 16;
            if (n == 0)
                continue;
            acc |= (uint64_t)(grp[j] & 0xFFFFu) << nbits;
            nbits += n;
            if (nbits < 8)
                continue;
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
        w->pos = pos;
        w->acc = acc;
        w->nbits = nbits;
        if (failed)
            w->failed = 1;
    }

    return w->failed ? PARC_ERR_LIMIT : PARC_OK;
}

parc_err parc_fse_decode(parc_br *r, const parc_fdec *d, uint8_t *out,
                         size_t count)
{
    if (count == 0)
        return PARC_OK;
    if (r->failed)
        return PARC_ERR_TRUNCATED;

    /* Local reader: hold acc/nbits/pos in registers across the whole loop and
     * top up from one 64-bit load per several symbols, instead of paying a
     * full parc_br_get (avail multiply + sticky-error branch + refill) per
     * symbol. table_log <= 12, so one >= 57-bit refill feeds >= 4 symbols and
     * the per-symbol cost drops to a table lookup, a shift, and one `nbits <
     * nb` compare. Bit-exact with the byte-at-a-time reader: the same whole
     * bytes are pulled in the same order, and (pos*8 - nbits) tracks
     * bits_consumed identically, so the written-back reader state is what the
     * original per-get path would have left for the downstream streams. The
     * sticky-error contract lets us test truncation once (per refill) rather
     * than per get. */
    const uint8_t *src = r->src;
    size_t len = r->len;
    size_t pos = r->pos;
    uint64_t acc = r->acc;
    unsigned nbits = r->nbits;
    unsigned tl = d->table_log;
    int trunc = 0;

    /* Ensure >= (need) bits (need <= tl <= 12) are buffered in acc, mirroring
     * parc_br_fill; if the buffer runs dry first, flag truncation. */
#define FSE_REFILL(need)                                                       \
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

    FSE_REFILL(tl);
    uint32_t state = (uint32_t)(acc & ((UINT64_C(1) << tl) - 1));
    acc >>= tl;
    nbits -= tl;

    for (size_t i = 0; !trunc && i < count; ++i) {
        out[i] = d->symbol[state];
        if (i + 1 < count) {
            unsigned nb = d->nbits[state];
            FSE_REFILL(nb);
            if (trunc)
                break;
            /* nb <= table_log <= 12, so the mask is well-defined; nb == 0
             * yields low == 0 with no branch. */
            uint32_t low = (uint32_t)(acc & ((UINT64_C(1) << nb) - 1));
            acc >>= nb;
            nbits -= nb;
            state = (uint32_t)d->new_state[state] + low;
        }
    }
#undef FSE_REFILL

    r->pos = pos;
    r->acc = acc;
    r->nbits = nbits;
    if (trunc) {
        r->failed = 1;
        return PARC_ERR_TRUNCATED;
    }
    return PARC_OK;
}
