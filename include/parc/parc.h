#ifndef PARC_PARC_H
#define PARC_PARC_H

#include <stdint.h>
#include <stdio.h>

#include "parc/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* parc codec public API, format v0 (docs/FORMAT.md). Streaming over stdio:
 * neither stream needs to be seekable, so files, pipes and fmemopen()/
 * open_memstream() buffers all work. Optionally multithreaded (see the
 * threads fields); a frame is bit-identical for every thread count, and
 * decoding accepts any frame regardless of how it was produced. */

/* Upper bound on the threads fields below. */
#define PARC_THREADS_MAX 512

/* Compression level range. Level selects match-search effort only; every
 * level produces a valid frame that any decoder reads. */
#define PARC_LEVEL_MIN 1
#define PARC_LEVEL_MAX 9
#define PARC_LEVEL_DEFAULT 3

/* Wire format selection (parc_copts.format). 0 requests the latest format;
 * the explicit values force a specific wire version for tooling and
 * cross-version tests. Any decoder reads either format. */
#define PARC_FORMAT_DEFAULT 0
#define PARC_FORMAT_V0 1 /* Huffman token stream */
#define PARC_FORMAT_V1 2 /* FSE sequence model + repeat offsets (latest) */

typedef struct parc_copts {
    /* log2 of the maximum block size: 0 for the default, else 12..24. The
     * default depends on level (the block is the match window): 20 (1 MiB)
     * for levels 1..3, 22 (4 MiB) for levels 4..9. Larger blocks improve
     * ratio (matches are block-local) at the cost of memory: compression uses
     * ~10x block size (~14x at chain levels, see level), decompression ~2x. */
    unsigned block_log;
    /* worker threads: 0 or 1 compresses on the calling thread; N >= 2
     * runs a pipeline of N compression workers plus a writer thread,
     * keeping 2N blocks in flight (memory ~N x 14x block size). Values
     * above PARC_THREADS_MAX are rejected with PARC_ERR_ARG. */
    unsigned threads;
    /* compression level: 0 for the default (PARC_LEVEL_DEFAULT), else
     * PARC_LEVEL_MIN..PARC_LEVEL_MAX. Level 1 is a fast greedy matcher;
     * higher levels widen the hash-chain match search (and use the chain
     * array, ~4x block size extra) for a better ratio at lower speed.
     * Out-of-range values are rejected with PARC_ERR_ARG. */
    unsigned level;
    /* wire format: 0 (PARC_FORMAT_DEFAULT) for the latest, or an explicit
     * PARC_FORMAT_V0 / PARC_FORMAT_V1. Out-of-range values are rejected with
     * PARC_ERR_ARG. Every format decodes on any build. */
    unsigned format;
} parc_copts;

typedef struct parc_dopts {
    /* worker threads for decompression; same contract as parc_copts.threads
     * (memory ~N x 4x block size, block size taken from the frame header). */
    unsigned threads;
} parc_dopts;

typedef struct parc_info {
    uint64_t raw_bytes;   /* content size */
    uint64_t frame_bytes; /* total compressed frame size */
    uint32_t blocks;
    uint32_t stored_blocks; /* blocks kept raw (incompressible) */
} parc_info;

/* Compress all of in (to EOF) into one frame on out. opts and info may be
 * NULL. Returns PARC_OK, PARC_ERR_ARG (bad opts), PARC_ERR_NOMEM (allocation
 * or thread creation failed), or PARC_ERR_IO (read or write failure). */
parc_err parc_compress_stream(FILE *in, FILE *out, const parc_copts *opts,
                              parc_info *info);

/* Decompress exactly one frame from in onto out, verifying everything
 * FORMAT.md requires (structure, per-block hashes, stream hash, index,
 * exact EOF after the frame). out == NULL verifies without writing.
 * opts and info may be NULL; info is filled on success. On any error,
 * output already written to out stays written (callers should discard
 * the file). */
parc_err parc_decompress_stream(FILE *in, FILE *out, const parc_dopts *opts,
                                parc_info *info);

#ifdef __cplusplus
}
#endif

#endif /* PARC_PARC_H */
