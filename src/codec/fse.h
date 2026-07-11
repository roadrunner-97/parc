#ifndef PARC_CODEC_FSE_H
#define PARC_CODEC_FSE_H

#include <stddef.h>
#include <stdint.h>

#include "parc/err.h"
#include "util/bitstream.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Table-driven ANS (FSE) entropy coder for the v1 packed block payload.
 *
 * A stream is a run of `count` symbols over the alphabet 0..max_symbol,
 * coded against a normalized frequency table whose counts sum to exactly
 * 1 << table_log. The encoder processes symbols in reverse (ANS is LIFO) and
 * lays its output groups down in *decode order*, so decoding is an ordinary
 * forward walk over parc_br — the same reader the Huffman stage uses. This
 * keeps the hot decode path on the well-tested forward bitstream and needs no
 * backward reader.
 *
 * Bit layout on the wire (all via the LSB-first parc_bw/parc_br): a table
 * description (parc_fse_write_table) followed by the coded groups
 * (initial state, then one renormalization group per symbol transition). */

enum {
    PARC_FSE_MAX_SYMS = 256,      /* literal alphabet is the widest */
    PARC_FSE_MIN_TABLELOG = 5,
    PARC_FSE_MAX_TABLELOG = 12,
    PARC_FSE_DEFAULT_TABLELOG = 11,
};

/* Pick a table_log for `count` symbols over 0..max_symbol. Result is in
 * [PARC_FSE_MIN_TABLELOG, PARC_FSE_MAX_TABLELOG] and always large enough that
 * every present symbol can be given a normalized count >= 1. count >= 1. */
unsigned parc_fse_tablelog(size_t count, unsigned max_symbol);

/* Normalize freq[0..max_symbol] (total > 0) to norm[0..max_symbol] summing to
 * exactly 1 << table_log, every present symbol (freq > 0) getting norm >= 1.
 * Requires table_log large enough (see parc_fse_tablelog); returns
 * PARC_ERR_LIMIT if the present-symbol count exceeds the table size. */
parc_err parc_fse_normalize(const uint32_t *freq, unsigned max_symbol,
                            unsigned table_log, int16_t *norm);

/* ---- built tables ---- */

typedef struct parc_fenc {
    uint32_t delta_nbits[PARC_FSE_MAX_SYMS];
    int32_t delta_find[PARC_FSE_MAX_SYMS];
    uint16_t state_table[1u << PARC_FSE_MAX_TABLELOG];
    unsigned table_log;
} parc_fenc;

typedef struct parc_fdec {
    uint16_t new_state[1u << PARC_FSE_MAX_TABLELOG];
    uint8_t symbol[1u << PARC_FSE_MAX_TABLELOG];
    uint8_t nbits[1u << PARC_FSE_MAX_TABLELOG];
    unsigned table_log;
} parc_fdec;

/* Build the encode table from a normalized count table. norm must sum to
 * 1 << table_log (as produced by parc_fse_normalize). */
void parc_fenc_build(parc_fenc *e, const int16_t *norm, unsigned max_symbol,
                     unsigned table_log);

/* Build and validate the decode table. Rejects (PARC_ERR_CORRUPT) a norm
 * table whose counts do not sum to 1 << table_log or fall outside
 * [0, 1 << table_log]. */
parc_err parc_fdec_build(parc_fdec *d, const int16_t *norm, unsigned max_symbol,
                         unsigned table_log);

/* ---- table (de)serialization ---- */

/* Emit the table description: table_log (4b), max_symbol (8b), then each
 * count in table_log+1 bits. */
void parc_fse_write_table(parc_bw *w, const int16_t *norm, unsigned max_symbol,
                          unsigned table_log);

/* Read a table description into norm[0..limit] (symbols past max_symbol
 * zeroed). Validates table_log range, max_symbol <= limit, the highest symbol
 * present, and the count sum. On success fills *max_symbol and *table_log. */
parc_err parc_fse_read_table(parc_br *r, int16_t *norm, unsigned *max_symbol,
                             unsigned *table_log, unsigned limit);

/* ---- symbol coding ---- */

/* Encode syms[0..count) (each <= max_symbol used to build e) into w. grp is
 * caller scratch holding at least `count` uint32_t. count may be 0 (no-op).
 * Returns PARC_ERR_LIMIT if the writer ran out of capacity. */
parc_err parc_fse_encode(parc_bw *w, const parc_fenc *e, const uint8_t *syms,
                         size_t count, uint32_t *grp);

/* Decode `count` symbols from r into out[0..count). count may be 0 (no-op).
 * Returns PARC_ERR_TRUNCATED if the stream ran short. */
parc_err parc_fse_decode(parc_br *r, const parc_fdec *d, uint8_t *out,
                         size_t count);

#ifdef __cplusplus
}
#endif

#endif /* PARC_CODEC_FSE_H */
