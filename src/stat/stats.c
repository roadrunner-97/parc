#include "stat/stats.h"

/* TODO(subagent): implement per the contract in src/stat/stats.h. */

struct parc_stats {
    int unused;
};

parc_err parc_stats_create(parc_stats **out)
{
    (void)out;
    return PARC_ERR_NOMEM;
}

void parc_stats_destroy(parc_stats *st)
{
    (void)st;
}

void parc_stats_reset(parc_stats *st)
{
    (void)st;
}

void parc_stats_update(parc_stats *st, const void *data, size_t len)
{
    (void)st;
    (void)data;
    (void)len;
}

uint64_t parc_stats_len(const parc_stats *st)
{
    (void)st;
    return 0;
}

double parc_stats_entropy_o0(const parc_stats *st)
{
    (void)st;
    return -1.0;
}

double parc_stats_entropy_o1(const parc_stats *st)
{
    (void)st;
    return -1.0;
}

double parc_stats_min_entropy(const parc_stats *st)
{
    (void)st;
    return -1.0;
}

double parc_stats_mean(const parc_stats *st)
{
    (void)st;
    return -1.0;
}

double parc_stats_chi2(const parc_stats *st, double *p_value)
{
    (void)st;
    (void)p_value;
    return -1.0;
}

double parc_stats_serial_corr(const parc_stats *st)
{
    (void)st;
    return -1.0;
}

double parc_stats_montecarlo_pi(const parc_stats *st)
{
    (void)st;
    return -1.0;
}

double parc_chi2_sf(double x, unsigned df)
{
    (void)x;
    (void)df;
    return -1.0;
}

parc_err parc_entropy_profile(const uint8_t *data, size_t len, size_t window,
                              size_t stride, double *out, size_t *out_n)
{
    (void)data;
    (void)len;
    (void)window;
    (void)stride;
    (void)out;
    (void)out_n;
    return PARC_ERR_ARG;
}
