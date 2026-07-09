#include "gen.h"

/* TODO(subagent): implement per the contract in tools/parcgen/gen.h. */

parc_err parc_gen_random(parc_rng *r, size_t len, parc_buf *out)
{
    (void)r;
    (void)len;
    (void)out;
    return PARC_ERR_NOMEM;
}

parc_err parc_gen_entropy(parc_rng *r, double target_bits, size_t len,
                          parc_buf *out)
{
    (void)r;
    (void)target_bits;
    (void)len;
    (void)out;
    return PARC_ERR_NOMEM;
}

parc_err parc_gen_runs(parc_rng *r, double mean_run, size_t len, parc_buf *out)
{
    (void)r;
    (void)mean_run;
    (void)len;
    (void)out;
    return PARC_ERR_NOMEM;
}

parc_err parc_gen_text(parc_rng *r, size_t len, parc_buf *out)
{
    (void)r;
    (void)len;
    (void)out;
    return PARC_ERR_NOMEM;
}

parc_err parc_gen_json_log(parc_rng *r, size_t len, parc_buf *out)
{
    (void)r;
    (void)len;
    (void)out;
    return PARC_ERR_NOMEM;
}

parc_err parc_gen_records(parc_rng *r, size_t rec_size, size_t len,
                          parc_buf *out)
{
    (void)r;
    (void)rec_size;
    (void)len;
    (void)out;
    return PARC_ERR_NOMEM;
}
