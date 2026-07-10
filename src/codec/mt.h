#ifndef PARC_CODEC_MT_H
#define PARC_CODEC_MT_H

#include <stddef.h>
#include <stdint.h>

#include "parc/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Generic ordered block pipeline, shared by the multithreaded compress and
 * decompress paths: the calling thread reads blocks in sequence into a ring
 * of slots, `threads` workers transform claimed slots in any order, and one
 * writer thread emits results back in sequence. 2 x threads slots are in
 * flight. On the first error from any callback the whole pipeline drains
 * and parc_mt_run returns that error; output already written stays
 * written. */

typedef struct parc_mt_slot {
    uint8_t *in;  /* filled by read(); max_block bytes */
    uint8_t *out; /* filled by work(); max_block bytes */
    uint32_t in_len;
    uint32_t out_len;
    uint8_t type;  /* block type; meaning owned by the callbacks */
    uint64_t hash; /* per-block hash; meaning owned by the callbacks */
} parc_mt_slot;

typedef struct parc_mt_ops {
    /* Produce the next block, called on the calling thread in sequence
     * order. Set s->in_len (0 = no block produced) plus whatever work()
     * needs, and *eof = 1 when no further block follows (possibly together
     * with a final block). */
    parc_err (*read)(void *ctx, parc_mt_slot *s, int *eof);
    /* Transform one slot, called on a worker thread; slots arrive in any
     * order and only s may be touched (ctx is shared and unlocked). */
    parc_err (*work)(void *ctx, void *wctx, parc_mt_slot *s);
    /* Consume a transformed slot, called on the writer thread in sequence
     * order. */
    parc_err (*write)(void *ctx, parc_mt_slot *s);
    /* Optional per-worker scratch, created/destroyed on the calling
     * thread. Both NULL, or both set; wctx_free must accept NULL (it is
     * called for workers whose init never ran when setup fails midway). */
    parc_err (*wctx_init)(void *ctx, void **wctx);
    void (*wctx_free)(void *wctx);
} parc_mt_ops;

/* Run the pipeline to eof or first error. threads in [2, PARC_THREADS_MAX];
 * max_block bounds every slot buffer. Returns the first error (thread
 * creation failure maps to PARC_ERR_NOMEM). */
parc_err parc_mt_run(const parc_mt_ops *ops, void *ctx, unsigned threads,
                     size_t max_block);

#ifdef __cplusplus
}
#endif

#endif /* PARC_CODEC_MT_H */
