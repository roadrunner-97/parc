#ifndef PARC_CODEC_BLOCK_H
#define PARC_CODEC_BLOCK_H

#include <stddef.h>
#include <stdint.h>

#include "parc/err.h"
#include "codec/lz.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Single-block codec: LZ tokens + canonical Huffman, packed-payload layout
 * per docs/FORMAT.md §2. Frame structure (headers, hashes, index) is the
 * frame layer's job. */

enum parc_btype {
    PARC_BLK_STORED = 0x00,
    PARC_BLK_PACKED = 0x01,
    PARC_BLK_END = 0xFF, /* end marker; not a block type on the API below */
};

/* Reusable compression scratch (hash table + token array), sized for
 * blocks up to max_block bytes. */
typedef struct parc_blk_cctx {
    uint32_t *htab;
    parc_tok *toks;
    size_t max_block;
} parc_blk_cctx;

parc_err parc_blk_cctx_init(parc_blk_cctx *cx, size_t max_block);
void parc_blk_cctx_free(parc_blk_cctx *cx);

/* Compress src[0..raw_len) into dst, raw_len in [1, cx->max_block] and
 * dst holding at least raw_len - 1 bytes. Returns PARC_BLK_PACKED with
 * *comp_len in [1, raw_len) on success, or PARC_BLK_STORED (*comp_len =
 * raw_len, dst untouched — the caller stores src verbatim) when the packed
 * form would not beat stored. */
int parc_blk_compress(parc_blk_cctx *cx, const uint8_t *src, uint32_t raw_len,
                      uint8_t *dst, uint32_t *comp_len);

/* Decode a packed payload comp[0..comp_len) into dst[0..raw_len), with
 * every validation FORMAT.md demands. Returns PARC_OK or PARC_ERR_CORRUPT;
 * on error dst contents are unspecified. Stored payloads never reach here
 * (the frame layer copies them directly). */
parc_err parc_blk_decompress(const uint8_t *comp, uint32_t comp_len,
                             uint8_t *dst, uint32_t raw_len);

#ifdef __cplusplus
}
#endif

#endif /* PARC_CODEC_BLOCK_H */
