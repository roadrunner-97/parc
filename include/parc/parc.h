#ifndef PARC_PARC_H
#define PARC_PARC_H

#include <stdint.h>
#include <stdio.h>

#include "parc/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* parc codec public API, format v0 (docs/FORMAT.md). Single-threaded,
 * streaming over stdio: neither stream needs to be seekable, so files,
 * pipes and fmemopen()/open_memstream() buffers all work. */

typedef struct parc_copts {
    /* log2 of the maximum block size: 0 for the default (20 → 1 MiB),
     * else 12..24. Larger blocks improve ratio (matches are block-local)
     * at the cost of memory: compression uses ~10x block size,
     * decompression ~2x. */
    unsigned block_log;
} parc_copts;

typedef struct parc_info {
    uint64_t raw_bytes;   /* content size */
    uint64_t frame_bytes; /* total compressed frame size */
    uint32_t blocks;
    uint32_t stored_blocks; /* blocks kept raw (incompressible) */
} parc_info;

/* Compress all of in (to EOF) into one frame on out. opts and info may be
 * NULL. Returns PARC_OK, PARC_ERR_ARG (bad opts), PARC_ERR_NOMEM, or
 * PARC_ERR_IO (read or write failure). */
parc_err parc_compress_stream(FILE *in, FILE *out, const parc_copts *opts,
                              parc_info *info);

/* Decompress exactly one frame from in onto out, verifying everything
 * FORMAT.md requires (structure, per-block hashes, stream hash, index,
 * exact EOF after the frame). out == NULL verifies without writing.
 * info may be NULL and is filled on success. On any error, output already
 * written to out stays written (callers should discard the file). */
parc_err parc_decompress_stream(FILE *in, FILE *out, parc_info *info);

#ifdef __cplusplus
}
#endif

#endif /* PARC_PARC_H */
