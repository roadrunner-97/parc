#include "parc/parc.h"

#include <stdlib.h>
#include <string.h>

#include "codec/block.h"
#include "util/buf.h"
#include "util/xxh64.h"

/* Frame layout constants per docs/FORMAT.md §1 */
static const uint8_t FRAME_MAGIC[4] = {'p', 'A', 'r', 'c'};
static const uint8_t END_MAGIC[4] = {'p', 'E', 'n', 'd'};
#define FRAME_VERSION 0
#define BLOCK_LOG_MIN 12
#define BLOCK_LOG_MAX 24
#define BLOCK_LOG_DEFAULT 20
#define BLOCK_HDR_BYTES 17 /* type + raw_len + comp_len + raw_hash */
#define INDEX_ENTRY_BYTES 16
#define TRAILER_FIXED_BYTES 29 /* end marker + count + total + hash + footer */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
}

static parc_err write_all(FILE *out, const void *p, size_t n)
{
    return fwrite(p, 1, n, out) == n ? PARC_OK : PARC_ERR_IO;
}

/* Read exactly n bytes: distinguishes clean EOF (TRUNCATED) from stream
 * errors (IO). */
static parc_err read_exact(FILE *in, void *p, size_t n)
{
    if (fread(p, 1, n, in) == n)
        return PARC_OK;
    return ferror(in) ? PARC_ERR_IO : PARC_ERR_TRUNCATED;
}

static void index_entry(uint8_t e[INDEX_ENTRY_BYTES], uint64_t offset,
                        uint32_t raw_len, uint32_t comp_len)
{
    put64(e, offset);
    put32(e + 8, raw_len);
    put32(e + 12, comp_len);
}

/* ---- compression ---- */

parc_err parc_compress_stream(FILE *in, FILE *out, const parc_copts *opts,
                              parc_info *info)
{
    unsigned bl = opts && opts->block_log ? opts->block_log : BLOCK_LOG_DEFAULT;
    if (bl < BLOCK_LOG_MIN || bl > BLOCK_LOG_MAX)
        return PARC_ERR_ARG;
    size_t bs = (size_t)1 << bl;

    parc_err err = PARC_ERR_NOMEM;
    uint8_t *raw = malloc(bs);
    uint8_t *payload = malloc(bs); /* packed payload cap is bs - 1 */
    parc_blk_cctx cx = {0};
    parc_buf index;
    parc_buf_init(&index);
    if (!raw || !payload || parc_blk_cctx_init(&cx, bs) != PARC_OK)
        goto done;

    uint8_t hdr[8] = {FRAME_MAGIC[0], FRAME_MAGIC[1], FRAME_MAGIC[2],
                      FRAME_MAGIC[3], FRAME_VERSION, 0, (uint8_t)bl, 0};
    err = write_all(out, hdr, sizeof hdr);
    if (err)
        goto done;

    parc_xxh64_state sh;
    parc_xxh64_init(&sh, 0);
    uint64_t total_raw = 0, offset = 8;
    uint32_t blocks = 0, stored = 0;

    for (;;) {
        size_t got = fread(raw, 1, bs, in);
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

        uint8_t bhdr[BLOCK_HDR_BYTES];
        bhdr[0] = (uint8_t)type;
        put32(bhdr + 1, raw_len);
        put32(bhdr + 5, comp_len);
        put64(bhdr + 9, parc_xxh64(raw, got, 0));
        err = write_all(out, bhdr, sizeof bhdr);
        if (!err)
            err = write_all(out, body, comp_len);
        if (err)
            goto done;

        uint8_t ie[INDEX_ENTRY_BYTES];
        index_entry(ie, offset, raw_len, comp_len);
        err = parc_buf_append(&index, ie, sizeof ie);
        if (err)
            goto done;

        parc_xxh64_update(&sh, raw, got);
        total_raw += got;
        offset += BLOCK_HDR_BYTES + comp_len;
        blocks++;
        stored += type == PARC_BLK_STORED;

        if (got < bs)
            break; /* short read == EOF (checked ferror above) */
    }

    uint64_t trailer_len =
        TRAILER_FIXED_BYTES + (uint64_t)INDEX_ENTRY_BYTES * blocks;
    uint8_t t[1 + 4];
    t[0] = PARC_BLK_END;
    put32(t + 1, blocks);
    err = write_all(out, t, sizeof t);
    if (!err)
        err = write_all(out, index.data, index.len);
    uint8_t tail[8 + 8 + 4 + 4];
    put64(tail, total_raw);
    put64(tail + 8, parc_xxh64_digest(&sh));
    put32(tail + 16, (uint32_t)trailer_len);
    memcpy(tail + 20, END_MAGIC, 4);
    if (!err)
        err = write_all(out, tail, sizeof tail);
    if (!err && fflush(out) != 0)
        err = PARC_ERR_IO;
    if (err)
        goto done;

    if (info) {
        info->raw_bytes = total_raw;
        info->frame_bytes = offset + trailer_len;
        info->blocks = blocks;
        info->stored_blocks = stored;
    }
    err = PARC_OK;
done:
    free(raw);
    free(payload);
    parc_blk_cctx_free(&cx);
    parc_buf_free(&index);
    return err;
}

/* ---- decompression / verification ---- */

parc_err parc_decompress_stream(FILE *in, FILE *out, parc_info *info)
{
    uint8_t hdr[8];
    parc_err err = read_exact(in, hdr, sizeof hdr);
    if (err)
        return err;
    if (memcmp(hdr, FRAME_MAGIC, 4) != 0)
        return PARC_ERR_CORRUPT;
    if (hdr[4] != FRAME_VERSION || hdr[5] != 0)
        return PARC_ERR_VERSION; /* unknown version or flags */
    unsigned bl = hdr[6];
    if (bl < BLOCK_LOG_MIN || bl > BLOCK_LOG_MAX || hdr[7] != 0)
        return PARC_ERR_CORRUPT;
    size_t bs = (size_t)1 << bl;

    uint8_t *cbuf = malloc(bs);
    uint8_t *raw = malloc(bs);
    parc_buf seen; /* index entries as read from the blocks, wire encoding */
    parc_buf_init(&seen);
    if (!cbuf || !raw) {
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
        int bad_lens =
            raw_len < 1 || raw_len > bs ||
            (type == PARC_BLK_STORED ? comp_len != raw_len
                                     : comp_len < 1 || comp_len >= raw_len);
        if (bad_lens) {
            err = PARC_ERR_CORRUPT;
            goto done;
        }

        err = read_exact(in, cbuf, comp_len);
        if (err)
            goto done;
        const uint8_t *body = cbuf;
        if (type == PARC_BLK_PACKED) {
            err = parc_blk_decompress(cbuf, comp_len, raw, raw_len);
            if (err)
                goto done;
            body = raw;
        }
        if (parc_xxh64(body, raw_len, 0) != want_hash) {
            err = PARC_ERR_CHECKSUM;
            goto done;
        }
        if (out && fwrite(body, 1, raw_len, out) != raw_len) {
            err = PARC_ERR_IO;
            goto done;
        }

        uint8_t ie[INDEX_ENTRY_BYTES];
        index_entry(ie, offset, raw_len, comp_len);
        err = parc_buf_append(&seen, ie, sizeof ie);
        if (err)
            goto done;

        parc_xxh64_update(&sh, body, raw_len);
        total_raw += raw_len;
        offset += BLOCK_HDR_BYTES + comp_len;
        blocks++;
        stored += type == PARC_BLK_STORED;
    }

    /* trailer: every field must match what the blocks said (§1.3) */
    uint8_t cnt[4];
    err = read_exact(in, cnt, sizeof cnt);
    if (err)
        goto done;
    if (get32(cnt) != blocks) {
        err = PARC_ERR_CORRUPT;
        goto done;
    }
    for (uint32_t i = 0; i < blocks; ++i) {
        uint8_t ie[INDEX_ENTRY_BYTES];
        err = read_exact(in, ie, sizeof ie);
        if (err)
            goto done;
        if (memcmp(ie, seen.data + (size_t)i * INDEX_ENTRY_BYTES,
                   INDEX_ENTRY_BYTES) != 0) {
            err = PARC_ERR_CORRUPT;
            goto done;
        }
    }
    uint8_t tail[8 + 8 + 4 + 4];
    err = read_exact(in, tail, sizeof tail);
    if (err)
        goto done;
    uint64_t trailer_len =
        TRAILER_FIXED_BYTES + (uint64_t)INDEX_ENTRY_BYTES * blocks;
    if (get64(tail) != total_raw || get32(tail + 16) != trailer_len ||
        memcmp(tail + 20, END_MAGIC, 4) != 0) {
        err = PARC_ERR_CORRUPT;
        goto done;
    }
    if (get64(tail + 8) != parc_xxh64_digest(&sh)) {
        err = PARC_ERR_CHECKSUM;
        goto done;
    }
    if (fgetc(in) != EOF) {
        err = PARC_ERR_CORRUPT; /* trailing bytes after the frame */
        goto done;
    }
    if (ferror(in)) {
        err = PARC_ERR_IO;
        goto done;
    }
    if (out && fflush(out) != 0) {
        err = PARC_ERR_IO;
        goto done;
    }

    if (info) {
        info->raw_bytes = total_raw;
        info->frame_bytes = offset + trailer_len;
        info->blocks = blocks;
        info->stored_blocks = stored;
    }
    err = PARC_OK;
done:
    free(cbuf);
    free(raw);
    parc_buf_free(&seen);
    return err;
}
