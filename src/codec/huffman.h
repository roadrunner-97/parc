#ifndef PARC_CODEC_HUFFMAN_H
#define PARC_CODEC_HUFFMAN_H

#include <stddef.h>
#include <stdint.h>

#include "parc/err.h"
#include "util/bitstream.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical Huffman coding as pinned in docs/FORMAT.md §2.3: code lengths
 * limited to 15 bits, codes assigned in (length, symbol) order, code bits
 * on the wire MSB-first within the LSB-first bitstream. No heap allocation;
 * all state lives in the caller's structs. */

enum {
    PARC_HUFF_MAX_SYMS = 282, /* main alphabet size; dist alphabet is 25 */
    PARC_HUFF_MAX_LEN = 15,
    /* Fast decode indexes a direct table on the first this-many code bits;
     * codes longer than this (rare, by construction rare symbols) fall back
     * to a bit-serial walk. Bounds the decoder's table to 1 << this. */
    PARC_HDEC_ROOT_BITS = 11,
};

/* Compute code lengths for freq[0..n), n in [1, PARC_HUFF_MAX_SYMS].
 * lens[s] = 0 for symbols with freq 0. The result is always a table valid
 * per FORMAT.md: empty (all freqs zero), degenerate (one symbol, length 1),
 * or complete (Kraft sum exactly 2^15) with max length 15. Optimal Huffman
 * lengths except where the 15-bit limit forces adjustment. */
void parc_huff_lens(const uint32_t *freq, unsigned n, uint8_t *lens);

/* ---- encoder ---- */

typedef struct parc_henc {
    uint16_t code[PARC_HUFF_MAX_SYMS]; /* bit-reversed, ready for parc_bw_put */
    uint8_t len[PARC_HUFF_MAX_SYMS];
} parc_henc;

/* Build canonical codes from lengths (as produced by parc_huff_lens or any
 * table valid per FORMAT.md). Symbols with len 0 get code 0/len 0. */
void parc_henc_init(parc_henc *e, const uint8_t *lens, unsigned n);

/* Emit symbol s (len[s] must be nonzero). */
static inline void parc_henc_put(const parc_henc *e, parc_bw *w, unsigned s)
{
    parc_bw_put(w, e->code[s], e->len[s]);
}

/* ---- decoder ---- */

typedef struct parc_hdec {
    uint16_t count[PARC_HUFF_MAX_LEN + 1];  /* codes of each length */
    uint16_t first[PARC_HUFF_MAX_LEN + 1];  /* first canonical code of len */
    uint16_t offset[PARC_HUFF_MAX_LEN + 1]; /* index into syms for that len */
    uint16_t syms[PARC_HUFF_MAX_SYMS];      /* symbols in canonical order */
    uint16_t nsyms;   /* number of present symbols; 0 = empty table */
    uint8_t maxlen;   /* longest present code; 0 iff empty */
    uint8_t root_bits; /* direct-table width = min(maxlen, ROOT_BITS) */
    /* Fast path: index by the next root_bits stream bits. Each cell packs
     * (symbol << 4) | len for a code of len <= root_bits; len == 0 means no
     * code (reject), the sentinel len means "longer than root_bits, walk
     * bit-serially". Codes' MSB-first wire order is baked in (bit-reversed),
     * matching the encoder in parc_henc_init. */
    uint16_t tbl[1u << PARC_HDEC_ROOT_BITS];
} parc_hdec;

/* Validate lens[0..n) per FORMAT.md §2.3 (complete, degenerate, or empty)
 * and build the decode table. Returns PARC_ERR_CORRUPT for any other
 * table. */
parc_err parc_hdec_init(parc_hdec *d, const uint8_t *lens, unsigned n);

/* Decode one symbol. Returns the symbol, or -1 if the bits do not resolve
 * to a code (corrupt) or the bitstream ran out (r->failed; caller maps to
 * an error either way). Must not be called on an empty table. */
int parc_hdec_get(const parc_hdec *d, parc_br *r);

/* Decode `count` symbols into out[0..count) with a register-held local reader
 * (one wide refill per several symbols, single root-table lookup per symbol,
 * rare long-code fallback), the libdeflate-style hot path — several times
 * faster than a parc_hdec_get loop. Returns PARC_ERR_CORRUPT on an invalid
 * code or PARC_ERR_TRUNCATED if the stream ran short; leaves r's state
 * consistent for the caller's downstream checks. count may be 0. Must not be
 * called on an empty table (d->nsyms > 0). */
parc_err parc_hdec_decode(const parc_hdec *d, parc_br *r, uint8_t *out,
                          size_t count);

#ifdef __cplusplus
}
#endif

#endif /* PARC_CODEC_HUFFMAN_H */
