#ifndef PARC_UTIL_PROF_H
#define PARC_UTIL_PROF_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Compile-time-gated per-stage timing for the encode/decode pipelines.
 *
 * Build with -DPARC_PROF (CMake: -DPARC_PROF=ON, or the `prof` preset) to
 * enable. When it is off every macro below expands to `((void)0)`, so there is
 * zero runtime cost and the hot builds are untouched.
 *
 * Timing is at *stage* granularity — a handful of clock reads per block, never
 * per symbol — so the CLOCK_MONOTONIC overhead is noise and the numbers are
 * undistorted. Counters are plain globals with no locking, so profile the
 * single-threaded path only (`parc -T 1`); the MT workers would race them and
 * the MT frame path is not instrumented (its report prints nothing).
 *
 * The stage set maps directly onto the pipeline: the frame layer owns the I/O
 * and hashing stages, the block layer owns match / transcode / entropy /
 * reconstruct. `bytes` is the data volume that stage actually chewed through
 * (input bytes for a pass over the block); pass 0 when a stage is entered more
 * than once per block and another entry already carries the block's bytes, so
 * the per-stage MB/s stays a true throughput rather than an N-times overcount. */

enum parc_prof_stage {
    PARC_PROF_IO_READ,        /* fread of a raw block (compress) / packed block (decompress) */
    PARC_PROF_IO_WRITE,       /* write_all / fwrite of the block body */
    PARC_PROF_LZ_MATCH,       /* parc_lz_greedy / parc_lz_chain / parc_lz_optimal */
    PARC_PROF_TRANSCODE,      /* v1 token stream -> literal run + sequence list */
    PARC_PROF_ENTROPY_ENCODE, /* Huffman/FSE table build + symbol emit + extra bits */
    PARC_PROF_ENTROPY_DECODE, /* FSE/Huffman table read + symbol decode */
    PARC_PROF_RECONSTRUCT,    /* extra-bits decode + offset resolve + match copy */
    PARC_PROF_BLOCK_HASH,     /* per-block xxh64 (header hash / verify) */
    PARC_PROF_STREAM_HASH,    /* streaming xxh64 digest update */
    PARC_PROF_STAGE_COUNT
};

#ifdef PARC_PROF

uint64_t parc_prof_now_ns(void);
void parc_prof_add(enum parc_prof_stage stage, uint64_t ns, uint64_t bytes);
void parc_prof_reset(void);
void parc_prof_report(FILE *to);

/* Scoped timing: BEGIN stamps a start into a uniquely-named local, END bills
 * the elapsed time to `stage` and adds `bytes` to its throughput tally. `name`
 * must be unique within its scope; a BEGIN/END pair must sit in the same scope. */
#define PARC_PROF_BEGIN(name) uint64_t parc__prof_##name = parc_prof_now_ns()
#define PARC_PROF_END(name, stage, bytes)                                      \
    parc_prof_add((stage), parc_prof_now_ns() - parc__prof_##name, (bytes))

#define PARC_PROF_RESET() parc_prof_reset()
#define PARC_PROF_REPORT(to) parc_prof_report(to)

#else /* !PARC_PROF */

#define PARC_PROF_BEGIN(name) ((void)0)
#define PARC_PROF_END(name, stage, bytes) ((void)0)
#define PARC_PROF_RESET() ((void)0)
#define PARC_PROF_REPORT(to) ((void)0)

#endif /* PARC_PROF */

#endif /* PARC_UTIL_PROF_H */
