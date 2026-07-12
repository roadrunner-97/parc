#ifndef PARC_CODEC_BLOCK_H
#define PARC_CODEC_BLOCK_H

#include <stddef.h>
#include <stdint.h>

#include "parc/err.h"
#include "codec/lz.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Single-block codec, packed-payload layout per docs/FORMAT.md. The wire
 * version selects the entropy stage: version 0 uses canonical Huffman over a
 * flat token stream (§2); version 1 uses FSE over a sequence model with
 * repeat-offset codes (§3). Frame structure (headers, hashes, index) is the
 * frame layer's job. */

enum parc_btype {
    PARC_BLK_STORED = 0x00,
    PARC_BLK_PACKED = 0x01,
    PARC_BLK_END = 0xFF, /* end marker; not a block type on the API below */
};

/* Reusable compression scratch (hash table + token array, plus the chain
 * array for chain levels), sized for blocks up to max_block bytes and fixed
 * to one compression level and wire version. The v1 scratch (literal buffer,
 * sequence arrays, FSE group buffer and encode table) is allocated only when
 * version == 1. */
typedef struct parc_blk_cctx {
    uint32_t *htab;
    uint32_t *prev; /* chain array, max_block entries; NULL at greedy levels */
    parc_tok *toks;
    size_t max_block;
    parc_lz_cfg cfg;
    unsigned version;
    /* optimal-parse DP scratch, PARC_OPT_CHUNK + 1 entries each; NULL unless
     * cfg.optimal (levels 8..9). alt holds the alternate encode candidate
     * (max_block bytes) for the best-of-two comparison. */
    uint64_t *opt_price;
    uint32_t *opt_len;
    uint32_t *opt_dist;
    uint32_t *opt_rep; /* recent-offset cache per DP node, 3*(CHUNK+1) entries */
    uint8_t *alt;
    /* v1 only (NULL at version 0) */
    uint8_t *lit;    /* literal bytes, max_block */
    uint8_t *ll_sym; /* per-sequence bucket symbols, max_block/4 + 1 */
    uint8_t *ml_sym;
    uint8_t *of_sym;
    uint32_t *ll_ex; /* per-sequence extra-bit payloads */
    uint32_t *ml_ex;
    uint32_t *of_ex;
    uint32_t *grp;   /* FSE encode group scratch, max_block */
    void *fenc;      /* parc_fenc, reused across streams */
} parc_blk_cctx;

/* Reusable decompression scratch for version-1 blocks (literal buffer,
 * sequence arrays, FSE decode table). Not needed for version-0 blocks. */
typedef struct parc_blk_dctx {
    uint8_t *lit;   /* literal bytes, max_block */
    uint8_t *sym;   /* FSE symbol scratch, max_block/4 + 1 */
    uint32_t *ll;   /* per-sequence litLen / matchLen / distance */
    uint32_t *ml;
    uint32_t *dist;
    size_t max_block;
    void *fdec; /* parc_fdec */
} parc_blk_dctx;

/* level must be in [1, PARC_LZ_LEVEL_MAX]; version in {0, 1}. */
parc_err parc_blk_cctx_init(parc_blk_cctx *cx, size_t max_block, unsigned level,
                            unsigned version);
void parc_blk_cctx_free(parc_blk_cctx *cx);

parc_err parc_blk_dctx_init(parc_blk_dctx *dx, size_t max_block);
void parc_blk_dctx_free(parc_blk_dctx *dx);

/* Compress src[0..raw_len) into dst, raw_len in [1, cx->max_block] and
 * dst holding at least raw_len - 1 bytes. Returns PARC_BLK_PACKED with
 * *comp_len in [1, raw_len) on success, or PARC_BLK_STORED (*comp_len =
 * raw_len, dst untouched — the caller stores src verbatim) when the packed
 * form would not beat stored. Encodes in cx->version's wire format. */
int parc_blk_compress(parc_blk_cctx *cx, const uint8_t *src, uint32_t raw_len,
                      uint8_t *dst, uint32_t *comp_len);

/* Decode a packed payload comp[0..comp_len) into dst[0..raw_len) in the given
 * wire version, with every validation FORMAT.md demands. dx must be non-NULL
 * for version 1 (it is unused for version 0). Returns PARC_OK or
 * PARC_ERR_CORRUPT; on error dst contents are unspecified. Stored payloads
 * never reach here (the frame layer copies them directly). */
parc_err parc_blk_decompress(parc_blk_dctx *dx, const uint8_t *comp,
                             uint32_t comp_len, uint8_t *dst, uint32_t raw_len,
                             unsigned version);

#ifdef __cplusplus
}
#endif

#endif /* PARC_CODEC_BLOCK_H */
