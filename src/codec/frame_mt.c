#include "parc/parc.h"

#include <stdlib.h>

#include "codec/block.h"
#include "codec/frame_int.h"
#include "codec/mt.h"
#include "util/buf.h"
#include "util/xxh64.h"

/* Multithreaded frame paths over the parc_mt pipeline. Thread ownership:
 * everything below the "reader-owned" / "writer-owned" markers is touched
 * only by that pipeline thread while the pipeline runs; parc_mt_run's
 * joins make it all coherent for the calling thread afterwards. Both
 * paths produce/demand frames bit-identical to the single-threaded ones
 * in frame.c. */

/* ---- compression ---- */

typedef struct cmt_ctx {
    FILE *in, *out;
    size_t bs;
    unsigned level;
    /* reader-owned */
    parc_xxh64_state sh; /* stream hash, over raw bytes in read order */
    uint64_t total_raw;
    /* writer-owned */
    parc_buf index;
    uint64_t offset;
    uint32_t blocks, stored;
} cmt_ctx;

static parc_err cmt_read(void *vctx, parc_mt_slot *s, int *eof)
{
    cmt_ctx *c = vctx;
    size_t got = fread(s->in, 1, c->bs, c->in);
    if (ferror(c->in))
        return PARC_ERR_IO;
    if (got < c->bs)
        *eof = 1; /* short read == EOF (checked ferror above) */
    s->in_len = (uint32_t)got;
    if (got) {
        parc_xxh64_update(&c->sh, s->in, got);
        c->total_raw += got;
    }
    return PARC_OK;
}

static parc_err cmt_work(void *vctx, void *wctx, parc_mt_slot *s)
{
    (void)vctx;
    s->type = (uint8_t)parc_blk_compress(wctx, s->in, s->in_len, s->out,
                                         &s->out_len);
    s->hash = parc_xxh64(s->in, s->in_len, 0);
    return PARC_OK;
}

static parc_err cmt_write(void *vctx, parc_mt_slot *s)
{
    cmt_ctx *c = vctx;
    const uint8_t *body = s->type == PARC_BLK_STORED ? s->in : s->out;

    uint8_t bhdr[BLOCK_HDR_BYTES];
    block_hdr(bhdr, s->type, s->in_len, s->out_len, s->hash);
    parc_err err = write_all(c->out, bhdr, sizeof bhdr);
    if (!err)
        err = write_all(c->out, body, s->out_len);
    if (err)
        return err;

    uint8_t ie[INDEX_ENTRY_BYTES];
    index_entry(ie, c->offset, s->in_len, s->out_len);
    err = parc_buf_append(&c->index, ie, sizeof ie);
    c->offset += BLOCK_HDR_BYTES + s->out_len;
    c->blocks++;
    c->stored += s->type == PARC_BLK_STORED;
    return err;
}

static parc_err cmt_wctx_init(void *vctx, void **wctx)
{
    cmt_ctx *c = vctx;
    parc_blk_cctx *cx = calloc(1, sizeof *cx);
    if (!cx)
        return PARC_ERR_NOMEM;
    parc_err err = parc_blk_cctx_init(cx, c->bs, c->level);
    if (err) {
        free(cx);
        return err;
    }
    *wctx = cx;
    return PARC_OK;
}

static void cmt_wctx_free(void *wctx)
{
    if (!wctx)
        return;
    parc_blk_cctx_free(wctx);
    free(wctx);
}

parc_err parc_frame_compress_mt(FILE *in, FILE *out, unsigned bl,
                                unsigned threads, unsigned level,
                                parc_info *info)
{
    static const parc_mt_ops ops = {cmt_read, cmt_work, cmt_write,
                                    cmt_wctx_init, cmt_wctx_free};
    cmt_ctx c = {0};
    c.in = in;
    c.out = out;
    c.bs = (size_t)1 << bl;
    c.level = level;
    c.offset = 8;
    parc_xxh64_init(&c.sh, 0);
    parc_buf_init(&c.index);

    parc_err err = parc_frame_write_header(out, bl);
    if (!err)
        err = parc_mt_run(&ops, &c, threads, c.bs);
    if (!err)
        err = parc_frame_write_trailer(out, &c.index, c.blocks, c.total_raw,
                                       parc_xxh64_digest(&c.sh));
    if (!err && info) {
        info->raw_bytes = c.total_raw;
        info->frame_bytes = c.offset + trailer_len(c.blocks);
        info->blocks = c.blocks;
        info->stored_blocks = c.stored;
    }
    parc_buf_free(&c.index);
    return err;
}

/* ---- decompression / verification ---- */

typedef struct dmt_ctx {
    FILE *in, *out; /* out NULL = verify only */
    size_t bs;
    /* reader-owned */
    parc_buf seen; /* index entries as read from the blocks, wire encoding */
    uint64_t offset, total_raw;
    uint32_t blocks, stored;
    uint64_t want_hash; /* trailer's stream hash; valid once eof reached */
    /* writer-owned */
    parc_xxh64_state sh; /* stream hash, over content bytes in write order */
} dmt_ctx;

static parc_err dmt_read(void *vctx, parc_mt_slot *s, int *eof)
{
    dmt_ctx *c = vctx;
    int type = fgetc(c->in);
    if (type == EOF)
        return ferror(c->in) ? PARC_ERR_IO : PARC_ERR_TRUNCATED;
    if (type == PARC_BLK_END) {
        /* Everything but the stream hash is checkable now; the hash needs
         * the writer to finish, so it is compared after the join. */
        *eof = 1;
        return parc_frame_check_trailer(c->in, &c->seen, c->blocks,
                                        c->total_raw, &c->want_hash);
    }
    if (type != PARC_BLK_STORED && type != PARC_BLK_PACKED)
        return PARC_ERR_CORRUPT;

    uint8_t bhdr[BLOCK_HDR_BYTES - 1];
    parc_err err = read_exact(c->in, bhdr, sizeof bhdr);
    if (err)
        return err;
    uint32_t raw_len = get32(bhdr);
    uint32_t comp_len = get32(bhdr + 4);
    if (!block_lens_ok(type, raw_len, comp_len, c->bs))
        return PARC_ERR_CORRUPT;
    err = read_exact(c->in, s->in, comp_len);
    if (err)
        return err;
    s->in_len = comp_len;
    s->out_len = raw_len;
    s->type = (uint8_t)type;
    s->hash = get64(bhdr + 8);

    uint8_t ie[INDEX_ENTRY_BYTES];
    index_entry(ie, c->offset, raw_len, comp_len);
    err = parc_buf_append(&c->seen, ie, sizeof ie);
    c->offset += BLOCK_HDR_BYTES + comp_len;
    c->total_raw += raw_len;
    c->blocks++;
    c->stored += type == PARC_BLK_STORED;
    return err;
}

static parc_err dmt_work(void *vctx, void *wctx, parc_mt_slot *s)
{
    (void)vctx;
    (void)wctx;
    const uint8_t *body = s->in;
    if (s->type == PARC_BLK_PACKED) {
        parc_err err = parc_blk_decompress(s->in, s->in_len, s->out,
                                           s->out_len);
        if (err)
            return err;
        body = s->out;
    }
    if (parc_xxh64(body, s->out_len, 0) != s->hash)
        return PARC_ERR_CHECKSUM;
    return PARC_OK;
}

static parc_err dmt_write(void *vctx, parc_mt_slot *s)
{
    dmt_ctx *c = vctx;
    const uint8_t *body = s->type == PARC_BLK_PACKED ? s->out : s->in;
    if (c->out && fwrite(body, 1, s->out_len, c->out) != s->out_len)
        return PARC_ERR_IO;
    parc_xxh64_update(&c->sh, body, s->out_len);
    return PARC_OK;
}

parc_err parc_frame_decompress_mt(FILE *in, FILE *out, unsigned threads,
                                  parc_info *info)
{
    static const parc_mt_ops ops = {dmt_read, dmt_work, dmt_write, NULL,
                                    NULL};
    dmt_ctx c = {0};
    c.in = in;
    c.out = out;
    c.offset = 8;
    parc_xxh64_init(&c.sh, 0);
    parc_buf_init(&c.seen);

    unsigned bl;
    parc_err err = parc_frame_read_header(in, &bl);
    if (err)
        return err;
    c.bs = (size_t)1 << bl;

    err = parc_mt_run(&ops, &c, threads, c.bs);
    /* err == PARC_OK implies the reader consumed and validated the whole
     * trailer, so want_hash is set. */
    if (!err && c.want_hash != parc_xxh64_digest(&c.sh))
        err = PARC_ERR_CHECKSUM;
    if (!err && out && fflush(out) != 0)
        err = PARC_ERR_IO;
    if (!err && info) {
        info->raw_bytes = c.total_raw;
        info->frame_bytes = c.offset + trailer_len(c.blocks);
        info->blocks = c.blocks;
        info->stored_blocks = c.stored;
    }
    parc_buf_free(&c.seen);
    return err;
}
