#include "parc/parc.h"

#include <stdlib.h>
#include <string.h>

#include "codec/block.h"
#include "codec/frame_int.h"
#include "util/buf.h"
#include "util/prof.h"
#include "util/xxh64.h"

/* ---- header and trailer, shared with frame_mt.c ---- */

parc_err parc_frame_write_header(FILE *out, unsigned bl, unsigned version)
{
    uint8_t hdr[8] = {FRAME_MAGIC[0], FRAME_MAGIC[1],  FRAME_MAGIC[2],
                      FRAME_MAGIC[3], (uint8_t)version, 0, (uint8_t)bl, 0};
    return write_all(out, hdr, sizeof hdr);
}

parc_err parc_frame_read_header(FILE *in, unsigned *bl, unsigned *version)
{
    uint8_t hdr[8];
    parc_err err = read_exact(in, hdr, sizeof hdr);
    if (err)
        return err;
    if (memcmp(hdr, FRAME_MAGIC, 4) != 0)
        return PARC_ERR_CORRUPT;
    if (hdr[4] > FRAME_VERSION_MAX || hdr[5] != 0)
        return PARC_ERR_VERSION; /* unknown version or flags */
    *version = hdr[4];
    *bl = hdr[6];
    if (*bl < BLOCK_LOG_MIN || *bl > BLOCK_LOG_MAX || hdr[7] != 0)
        return PARC_ERR_CORRUPT;
    return PARC_OK;
}

/* Map a parc_copts.format request to a wire version, or -1 if invalid. */
static int wire_version(unsigned format)
{
    switch (format) {
    case PARC_FORMAT_DEFAULT:
    case PARC_FORMAT_V2:
        return FRAME_VERSION_V2;
    case PARC_FORMAT_V1:
        return FRAME_VERSION_V1;
    case PARC_FORMAT_V0:
        return FRAME_VERSION_V0;
    default:
        return -1;
    }
}

parc_err parc_frame_write_trailer(FILE *out, const parc_buf *index,
                                  uint32_t blocks, uint64_t total_raw,
                                  uint64_t digest)
{
    uint8_t t[1 + 4];
    t[0] = PARC_BLK_END;
    put32(t + 1, blocks);
    parc_err err = write_all(out, t, sizeof t);
    if (!err)
        err = write_all(out, index->data, index->len);
    uint8_t tail[8 + 8 + 4 + 4];
    put64(tail, total_raw);
    put64(tail + 8, digest);
    put32(tail + 16, (uint32_t)trailer_len(blocks));
    memcpy(tail + 20, END_MAGIC, 4);
    if (!err)
        err = write_all(out, tail, sizeof tail);
    if (!err && fflush(out) != 0)
        err = PARC_ERR_IO;
    return err;
}

parc_err parc_frame_check_trailer(FILE *in, const parc_buf *seen,
                                  uint32_t blocks, uint64_t total_raw,
                                  uint64_t *want_hash)
{
    /* every field must match what the blocks said (§1.3) */
    uint8_t cnt[4];
    parc_err err = read_exact(in, cnt, sizeof cnt);
    if (err)
        return err;
    if (get32(cnt) != blocks)
        return PARC_ERR_CORRUPT;
    for (uint32_t i = 0; i < blocks; ++i) {
        uint8_t ie[INDEX_ENTRY_BYTES];
        err = read_exact(in, ie, sizeof ie);
        if (err)
            return err;
        if (memcmp(ie, seen->data + (size_t)i * INDEX_ENTRY_BYTES,
                   INDEX_ENTRY_BYTES) != 0)
            return PARC_ERR_CORRUPT;
    }
    uint8_t tail[8 + 8 + 4 + 4];
    err = read_exact(in, tail, sizeof tail);
    if (err)
        return err;
    if (get64(tail) != total_raw || get32(tail + 16) != trailer_len(blocks) ||
        memcmp(tail + 20, END_MAGIC, 4) != 0)
        return PARC_ERR_CORRUPT;
    if (fgetc(in) != EOF)
        return PARC_ERR_CORRUPT; /* trailing bytes after the frame */
    if (ferror(in))
        return PARC_ERR_IO;
    *want_hash = get64(tail + 8);
    return PARC_OK;
}

/* ---- compression ---- */

parc_err parc_compress_stream(FILE *in, FILE *out, const parc_copts *opts,
                              parc_info *info)
{
    unsigned threads = opts ? opts->threads : 0;
    if (threads > PARC_THREADS_MAX)
        return PARC_ERR_ARG;
    unsigned level = opts && opts->level ? opts->level : PARC_LEVEL_DEFAULT;
    if (level > PARC_LEVEL_MAX)
        return PARC_ERR_ARG;
    /* The block is the match window (matches are block-local, docs/FORMAT.md),
     * so the default block size grows with level: fast levels keep the small
     * 1 MiB window (they don't exploit a larger one and it costs memory and
     * speed), higher levels widen it to 4 MiB for more and longer matches. An
     * explicit opts->block_log overrides this. */
    unsigned bl = opts && opts->block_log
                      ? opts->block_log
                      : (level >= BLOCK_LOG_HIGH_LEVEL ? BLOCK_LOG_DEFAULT_HIGH
                                                       : BLOCK_LOG_DEFAULT);
    if (bl < BLOCK_LOG_MIN || bl > BLOCK_LOG_MAX)
        return PARC_ERR_ARG;
    int version = wire_version(opts ? opts->format : PARC_FORMAT_DEFAULT);
    if (version < 0)
        return PARC_ERR_ARG;
    if (threads > 1)
        return parc_frame_compress_mt(in, out, bl, threads, level,
                                      (unsigned)version, info);
    size_t bs = (size_t)1 << bl;
    PARC_PROF_RESET();

    parc_err err = PARC_ERR_NOMEM;
    uint8_t *raw = malloc(bs);
    uint8_t *payload = malloc(bs); /* packed payload cap is bs - 1 */
    parc_blk_cctx cx = {0};
    parc_buf index;
    parc_buf_init(&index);
    if (!raw || !payload ||
        parc_blk_cctx_init(&cx, bs, level, (unsigned)version) != PARC_OK)
        goto done;

    err = parc_frame_write_header(out, bl, (unsigned)version);
    if (err)
        goto done;

    parc_xxh64_state sh;
    parc_xxh64_init(&sh, 0);
    uint64_t total_raw = 0, offset = 8;
    uint32_t blocks = 0, stored = 0;

    for (;;) {
        PARC_PROF_BEGIN(rd);
        size_t got = fread(raw, 1, bs, in);
        PARC_PROF_END(rd, PARC_PROF_IO_READ, got);
        if (ferror(in)) {
            err = PARC_ERR_IO;
            goto done;
        }
        if (got == 0)
            break; /* EOF; empty content yields zero blocks */

        uint32_t raw_len = (uint32_t)got;
        uint32_t comp_len;
        int type = parc_blk_compress(&cx, raw, raw_len, payload, &comp_len);
        const uint8_t *body = type == PARC_BLK_STORED ? raw : payload;

        PARC_PROF_BEGIN(bh);
        uint64_t bhash = parc_xxh64(raw, got, 0);
        PARC_PROF_END(bh, PARC_PROF_BLOCK_HASH, got);
        uint8_t bhdr[BLOCK_HDR_BYTES];
        block_hdr(bhdr, (uint8_t)type, raw_len, comp_len, bhash);
        PARC_PROF_BEGIN(wr);
        err = write_all(out, bhdr, sizeof bhdr);
        if (!err)
            err = write_all(out, body, comp_len);
        PARC_PROF_END(wr, PARC_PROF_IO_WRITE, comp_len);
        if (err)
            goto done;

        uint8_t ie[INDEX_ENTRY_BYTES];
        index_entry(ie, offset, raw_len, comp_len);
        err = parc_buf_append(&index, ie, sizeof ie);
        if (err)
            goto done;

        PARC_PROF_BEGIN(sh);
        parc_xxh64_update(&sh, raw, got);
        PARC_PROF_END(sh, PARC_PROF_STREAM_HASH, got);
        total_raw += got;
        offset += BLOCK_HDR_BYTES + comp_len;
        blocks++;
        stored += type == PARC_BLK_STORED;

        if (got < bs)
            break; /* short read == EOF (checked ferror above) */
    }

    err = parc_frame_write_trailer(out, &index, blocks, total_raw,
                                   parc_xxh64_digest(&sh));
    if (err)
        goto done;

    if (info) {
        info->raw_bytes = total_raw;
        info->frame_bytes = offset + trailer_len(blocks);
        info->blocks = blocks;
        info->stored_blocks = stored;
    }
    err = PARC_OK;
done:
    PARC_PROF_REPORT(stderr);
    free(raw);
    free(payload);
    parc_blk_cctx_free(&cx);
    parc_buf_free(&index);
    return err;
}

/* ---- decompression / verification ---- */

parc_err parc_decompress_stream(FILE *in, FILE *out, const parc_dopts *opts,
                                parc_info *info)
{
    unsigned threads = opts ? opts->threads : 0;
    if (threads > PARC_THREADS_MAX)
        return PARC_ERR_ARG;
    if (threads > 1)
        return parc_frame_decompress_mt(in, out, threads, info);

    unsigned bl, version;
    parc_err err = parc_frame_read_header(in, &bl, &version);
    if (err)
        return err;
    size_t bs = (size_t)1 << bl;
    PARC_PROF_RESET();

    uint8_t *cbuf = malloc(bs);
    uint8_t *raw = malloc(bs + PARC_WILDCOPY_SLACK); /* wildcopy tail slack */
    parc_blk_dctx dx = {0}; /* v1 scratch; allocated only for version 1 */
    parc_buf seen; /* index entries as read from the blocks, wire encoding */
    parc_buf_init(&seen);
    if (!cbuf || !raw) {
        err = PARC_ERR_NOMEM;
        goto done;
    }
    if ((version == FRAME_VERSION_V1 || version == FRAME_VERSION_V2) &&
        parc_blk_dctx_init(&dx, bs) != PARC_OK) {
        err = PARC_ERR_NOMEM;
        goto done;
    }

    parc_xxh64_state sh;
    parc_xxh64_init(&sh, 0);
    uint64_t total_raw = 0, offset = 8;
    uint32_t blocks = 0, stored = 0;

    for (;;) {
        int type = fgetc(in);
        if (type == EOF) {
            err = ferror(in) ? PARC_ERR_IO : PARC_ERR_TRUNCATED;
            goto done;
        }
        if (type == PARC_BLK_END)
            break;
        if (type != PARC_BLK_STORED && type != PARC_BLK_PACKED) {
            err = PARC_ERR_CORRUPT;
            goto done;
        }

        uint8_t bhdr[BLOCK_HDR_BYTES - 1];
        err = read_exact(in, bhdr, sizeof bhdr);
        if (err)
            goto done;
        uint32_t raw_len = get32(bhdr);
        uint32_t comp_len = get32(bhdr + 4);
        uint64_t want_hash = get64(bhdr + 8);
        if (!block_lens_ok(type, raw_len, comp_len, bs)) {
            err = PARC_ERR_CORRUPT;
            goto done;
        }

        PARC_PROF_BEGIN(rd);
        err = read_exact(in, cbuf, comp_len);
        PARC_PROF_END(rd, PARC_PROF_IO_READ, comp_len);
        if (err)
            goto done;
        const uint8_t *body = cbuf;
        if (type == PARC_BLK_PACKED) {
            err = parc_blk_decompress(&dx, cbuf, comp_len, raw, raw_len,
                                      version);
            if (err)
                goto done;
            body = raw;
        }
        PARC_PROF_BEGIN(bh);
        uint64_t got_hash = parc_xxh64(body, raw_len, 0);
        PARC_PROF_END(bh, PARC_PROF_BLOCK_HASH, raw_len);
        if (got_hash != want_hash) {
            err = PARC_ERR_CHECKSUM;
            goto done;
        }
        PARC_PROF_BEGIN(wr);
        int wr_ok = !out || fwrite(body, 1, raw_len, out) == raw_len;
        PARC_PROF_END(wr, PARC_PROF_IO_WRITE, out ? raw_len : 0);
        if (!wr_ok) {
            err = PARC_ERR_IO;
            goto done;
        }

        uint8_t ie[INDEX_ENTRY_BYTES];
        index_entry(ie, offset, raw_len, comp_len);
        err = parc_buf_append(&seen, ie, sizeof ie);
        if (err)
            goto done;

        PARC_PROF_BEGIN(sh);
        parc_xxh64_update(&sh, body, raw_len);
        PARC_PROF_END(sh, PARC_PROF_STREAM_HASH, raw_len);
        total_raw += raw_len;
        offset += BLOCK_HDR_BYTES + comp_len;
        blocks++;
        stored += type == PARC_BLK_STORED;
    }

    uint64_t want_hash;
    err = parc_frame_check_trailer(in, &seen, blocks, total_raw, &want_hash);
    if (err)
        goto done;
    if (want_hash != parc_xxh64_digest(&sh)) {
        err = PARC_ERR_CHECKSUM;
        goto done;
    }
    if (out && fflush(out) != 0) {
        err = PARC_ERR_IO;
        goto done;
    }

    if (info) {
        info->raw_bytes = total_raw;
        info->frame_bytes = offset + trailer_len(blocks);
        info->blocks = blocks;
        info->stored_blocks = stored;
    }
    err = PARC_OK;
done:
    PARC_PROF_REPORT(stderr);
    free(cbuf);
    free(raw);
    parc_blk_dctx_free(&dx);
    parc_buf_free(&seen);
    return err;
}
