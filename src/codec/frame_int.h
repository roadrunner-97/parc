#ifndef PARC_CODEC_FRAME_INT_H
#define PARC_CODEC_FRAME_INT_H

#include <stdint.h>
#include <stdio.h>

#include "parc/parc.h"
#include "codec/block.h"
#include "util/buf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Frame-layer internals shared by the single-threaded (frame.c) and
 * multithreaded (frame_mt.c) paths: wire constants and encoders per
 * docs/FORMAT.md §1, plus the header/trailer read and write logic. Both
 * paths must produce and demand bit-identical frames. */

static const uint8_t FRAME_MAGIC[4] = {'p', 'A', 'r', 'c'};
static const uint8_t END_MAGIC[4] = {'p', 'E', 'n', 'd'};
#define FRAME_VERSION_V0 0 /* Huffman over a flat token stream */
#define FRAME_VERSION_V1 1 /* FSE sequence model + repeat offsets */
#define FRAME_VERSION_V2 2 /* v1 sequence model + Huffman literal stream */
#define FRAME_VERSION_MAX 2
#define BLOCK_LOG_MIN 12
#define BLOCK_LOG_MAX 24
#define BLOCK_LOG_DEFAULT 20      /* fast levels: 1 MiB window */
#define BLOCK_LOG_DEFAULT_HIGH 22 /* levels >= HIGH_LEVEL: 4 MiB window */
#define BLOCK_LOG_HIGH_LEVEL 4    /* first level to widen the default window */
#define BLOCK_HDR_BYTES 17 /* type + raw_len + comp_len + raw_hash */
#define INDEX_ENTRY_BYTES 16
#define TRAILER_FIXED_BYTES 29 /* end marker + count + total + hash + footer */

static inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static inline uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static inline uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
}

static inline parc_err write_all(FILE *out, const void *p, size_t n)
{
    return fwrite(p, 1, n, out) == n ? PARC_OK : PARC_ERR_IO;
}

/* Read exactly n bytes: distinguishes clean EOF (TRUNCATED) from stream
 * errors (IO). */
static inline parc_err read_exact(FILE *in, void *p, size_t n)
{
    if (fread(p, 1, n, in) == n)
        return PARC_OK;
    return ferror(in) ? PARC_ERR_IO : PARC_ERR_TRUNCATED;
}

static inline void index_entry(uint8_t e[INDEX_ENTRY_BYTES], uint64_t offset,
                               uint32_t raw_len, uint32_t comp_len)
{
    put64(e, offset);
    put32(e + 8, raw_len);
    put32(e + 12, comp_len);
}

static inline void block_hdr(uint8_t h[BLOCK_HDR_BYTES], uint8_t type,
                             uint32_t raw_len, uint32_t comp_len,
                             uint64_t raw_hash)
{
    h[0] = type;
    put32(h + 1, raw_len);
    put32(h + 5, comp_len);
    put64(h + 9, raw_hash);
}

/* The length invariants a block header MUST satisfy (§1.2). */
static inline int block_lens_ok(int type, uint32_t raw_len, uint32_t comp_len,
                                size_t block_size)
{
    if (raw_len < 1 || raw_len > block_size)
        return 0;
    return type == PARC_BLK_STORED ? comp_len == raw_len
                                   : comp_len >= 1 && comp_len < raw_len;
}

static inline uint64_t trailer_len(uint32_t blocks)
{
    return TRAILER_FIXED_BYTES + (uint64_t)INDEX_ENTRY_BYTES * blocks;
}

/* Write the 8-byte frame header. bl and version must already be validated. */
parc_err parc_frame_write_header(FILE *out, unsigned bl, unsigned version);

/* Read and validate the 8-byte frame header, yielding block_log and the wire
 * version (0 or 1). */
parc_err parc_frame_read_header(FILE *in, unsigned *bl, unsigned *version);

/* Write end marker, index, and trailer, and flush. digest is the stream
 * hash of all content bytes. */
parc_err parc_frame_write_trailer(FILE *out, const parc_buf *index,
                                  uint32_t blocks, uint64_t total_raw,
                                  uint64_t digest);

/* Read and validate everything after the end-marker byte against what the
 * blocks declared (§1.3): count, every index entry (seen holds the wire
 * encoding built while reading blocks), total, trailer_len, footer magic,
 * and exact EOF after the frame. The stream-hash field is returned in
 * *want_hash for the caller to compare (it may not be known yet on the
 * multithreaded path). */
parc_err parc_frame_check_trailer(FILE *in, const parc_buf *seen,
                                  uint32_t blocks, uint64_t total_raw,
                                  uint64_t *want_hash);

/* Multithreaded paths (frame_mt.c); threads >= 2, bl and version validated. */
parc_err parc_frame_compress_mt(FILE *in, FILE *out, unsigned bl,
                                unsigned threads, unsigned level,
                                unsigned version, parc_info *info);
parc_err parc_frame_decompress_mt(FILE *in, FILE *out, unsigned threads,
                                  parc_info *info);

#ifdef __cplusplus
}
#endif

#endif /* PARC_CODEC_FRAME_INT_H */
