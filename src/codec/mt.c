#include "codec/mt.h"

#include <pthread.h>
#include <stdlib.h>

#include "parc/parc.h"

/* Slot lifecycle, tracked with three sequence counters (written <=
 * claimed <= loaded always):
 *
 *   free    slot seq is free while seq - written < nslots
 *   loaded  reader filled it and published seq < loaded
 *   claimed a worker owns it (seq < claimed) and runs work() unlocked
 *   done    done[seq % nslots] set; writer may consume it
 *   written writer ran write() and recycled it (seq < written)
 *
 * The reader fills slots in order and the writer recycles them in order,
 * so "slot (loaded % nslots) is free" reduces to loaded - written <
 * nslots. Workers claim in order but finish in any order; done[] carries
 * the out-of-order completions to the in-order writer. */

typedef struct mt_engine {
    const parc_mt_ops *ops;
    void *ctx;
    parc_mt_slot *slots;
    uint8_t *done;
    unsigned nslots;
    pthread_mutex_t mu;
    pthread_cond_t cv_free;   /* writer recycled a slot; reader waits */
    pthread_cond_t cv_loaded; /* reader published a slot; workers wait */
    pthread_cond_t cv_done;   /* worker completed a slot; writer waits */
    uint64_t loaded;
    uint64_t claimed;
    uint64_t written;
    int eof;      /* reader finished; loaded is final */
    parc_err err; /* first error; sticky */
} mt_engine;

typedef struct mt_worker {
    mt_engine *e;
    void *wctx;
    pthread_t tid;
} mt_worker;

/* Record the first error and wake every waiter so all threads drain. */
static void mt_fail_locked(mt_engine *e, parc_err err)
{
    if (!e->err)
        e->err = err;
    pthread_cond_broadcast(&e->cv_free);
    pthread_cond_broadcast(&e->cv_loaded);
    pthread_cond_broadcast(&e->cv_done);
}

static void *mt_worker_main(void *arg)
{
    mt_worker *w = arg;
    mt_engine *e = w->e;
    pthread_mutex_lock(&e->mu);
    for (;;) {
        while (!e->err && !e->eof && e->claimed == e->loaded)
            pthread_cond_wait(&e->cv_loaded, &e->mu);
        if (e->err || e->claimed == e->loaded)
            break; /* error, or eof with everything claimed */
        uint64_t seq = e->claimed++;
        parc_mt_slot *s = &e->slots[seq % e->nslots];
        pthread_mutex_unlock(&e->mu);
        parc_err err = e->ops->work(e->ctx, w->wctx, s);
        pthread_mutex_lock(&e->mu);
        if (err) {
            mt_fail_locked(e, err);
            break;
        }
        e->done[seq % e->nslots] = 1;
        pthread_cond_signal(&e->cv_done);
    }
    pthread_mutex_unlock(&e->mu);
    return NULL;
}

static void *mt_writer_main(void *arg)
{
    mt_engine *e = arg;
    pthread_mutex_lock(&e->mu);
    for (;;) {
        unsigned i = (unsigned)(e->written % e->nslots);
        while (!e->err && !e->done[i] &&
               !(e->eof && e->written == e->loaded))
            pthread_cond_wait(&e->cv_done, &e->mu);
        if (e->err || !e->done[i])
            break; /* error, or eof with everything written */
        e->done[i] = 0;
        pthread_mutex_unlock(&e->mu);
        parc_err err = e->ops->write(e->ctx, &e->slots[i]);
        pthread_mutex_lock(&e->mu);
        if (err) {
            mt_fail_locked(e, err);
            break;
        }
        e->written++;
        pthread_cond_signal(&e->cv_free);
    }
    pthread_mutex_unlock(&e->mu);
    return NULL;
}

/* The reader loop, run on the calling thread. */
static void mt_read_all(mt_engine *e)
{
    pthread_mutex_lock(&e->mu);
    while (!e->err && !e->eof) {
        while (!e->err && e->loaded - e->written >= e->nslots)
            pthread_cond_wait(&e->cv_free, &e->mu);
        if (e->err)
            break;
        parc_mt_slot *s = &e->slots[e->loaded % e->nslots];
        pthread_mutex_unlock(&e->mu);
        int eof = 0;
        s->in_len = 0;
        parc_err err = e->ops->read(e->ctx, s, &eof);
        pthread_mutex_lock(&e->mu);
        if (err) {
            mt_fail_locked(e, err);
            break;
        }
        if (s->in_len > 0) {
            e->loaded++;
            pthread_cond_signal(&e->cv_loaded);
        }
        if (eof) {
            e->eof = 1;
            pthread_cond_broadcast(&e->cv_loaded);
            pthread_cond_broadcast(&e->cv_done);
        }
    }
    pthread_mutex_unlock(&e->mu);
}

parc_err parc_mt_run(const parc_mt_ops *ops, void *ctx, unsigned threads,
                     size_t max_block)
{
    if (threads < 2 || threads > PARC_THREADS_MAX)
        return PARC_ERR_ARG;

    mt_engine e = {0};
    e.ops = ops;
    e.ctx = ctx;
    e.nslots = threads * 2;
    pthread_mutex_init(&e.mu, NULL);
    pthread_cond_init(&e.cv_free, NULL);
    pthread_cond_init(&e.cv_loaded, NULL);
    pthread_cond_init(&e.cv_done, NULL);

    parc_err err = PARC_ERR_NOMEM;
    mt_worker *ws = calloc(threads, sizeof *ws);
    e.slots = calloc(e.nslots, sizeof *e.slots);
    e.done = calloc(e.nslots, 1);
    if (!ws || !e.slots || !e.done)
        goto done;
    for (unsigned i = 0; i < e.nslots; ++i) {
        e.slots[i].in = malloc(max_block);
        e.slots[i].out = malloc(max_block);
        if (!e.slots[i].in || !e.slots[i].out)
            goto done;
    }
    for (unsigned i = 0; i < threads; ++i) {
        ws[i].e = &e;
        if (ops->wctx_init && (err = ops->wctx_init(ctx, &ws[i].wctx)) != 0)
            goto done;
    }

    /* Spawn writer, then workers; on any failure poison the pipeline so
     * already-running threads drain, and join whatever was created. */
    err = PARC_ERR_NOMEM;
    pthread_t writer;
    int have_writer = pthread_create(&writer, NULL, mt_writer_main, &e) == 0;
    unsigned nworkers = 0;
    if (have_writer)
        for (; nworkers < threads; ++nworkers)
            if (pthread_create(&ws[nworkers].tid, NULL, mt_worker_main,
                               &ws[nworkers]) != 0)
                break;

    if (have_writer && nworkers == threads) {
        mt_read_all(&e);
    } else {
        pthread_mutex_lock(&e.mu);
        e.eof = 1;
        mt_fail_locked(&e, PARC_ERR_NOMEM);
        pthread_mutex_unlock(&e.mu);
    }

    for (unsigned i = 0; i < nworkers; ++i)
        pthread_join(ws[i].tid, NULL);
    if (have_writer)
        pthread_join(writer, NULL);
    err = e.err;

done:
    if (ws && ops->wctx_free)
        for (unsigned i = 0; i < threads; ++i)
            ops->wctx_free(ws[i].wctx);
    if (e.slots)
        for (unsigned i = 0; i < e.nslots; ++i) {
            free(e.slots[i].in);
            free(e.slots[i].out);
        }
    free(e.slots);
    free(e.done);
    free(ws);
    pthread_mutex_destroy(&e.mu);
    pthread_cond_destroy(&e.cv_free);
    pthread_cond_destroy(&e.cv_loaded);
    pthread_cond_destroy(&e.cv_done);
    return err;
}
