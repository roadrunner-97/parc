#include "stat/stats.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Streaming statistical accumulator. All state needed to make every result
 * independent of update chunking lives here: the order-1 pair table plus a
 * carried "previous byte", running sums for serial correlation, and a
 * carried partial 6-byte group for the Monte Carlo estimator. */
struct parc_stats {
    uint64_t hist[256];
    uint64_t (*pair)[256]; /* heap-allocated 256x256 (prev, next) counts */

    uint64_t n; /* total bytes consumed */

    int has_first;
    uint8_t first_byte;
    int has_prev;
    uint8_t prev_byte;

    uint64_t sum_u;    /* sum of byte values */
    uint64_t sum_u2;   /* sum of byte values squared */
    uint64_t t1_pairs; /* sum_{i=0}^{n-2} u_i * u_{i+1} (no wraparound) */

    uint8_t mc_carry[6];
    unsigned mc_carry_n;
    uint64_t mc_points;
    uint64_t mc_inside;
};

parc_err parc_stats_create(parc_stats **out)
{
    parc_stats *st = (parc_stats *)calloc(1, sizeof(*st));
    if (st == NULL)
        return PARC_ERR_NOMEM;

    st->pair = (uint64_t (*)[256])calloc(256, sizeof(*st->pair));
    if (st->pair == NULL) {
        free(st);
        return PARC_ERR_NOMEM;
    }

    *out = st;
    return PARC_OK;
}

void parc_stats_destroy(parc_stats *st)
{
    if (st == NULL)
        return;
    free(st->pair);
    free(st);
}

void parc_stats_reset(parc_stats *st)
{
    uint64_t (*pair)[256] = st->pair;
    memset(pair, 0, 256 * sizeof(*pair));
    memset(st, 0, sizeof(*st));
    st->pair = pair;
}

void parc_stats_update(parc_stats *st, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    for (size_t i = 0; i < len; ++i) {
        uint8_t b = p[i];

        st->hist[b]++;
        st->n++;
        st->sum_u += b;
        st->sum_u2 += (uint64_t)b * (uint64_t)b;

        if (!st->has_first) {
            st->has_first = 1;
            st->first_byte = b;
        }
        if (st->has_prev) {
            st->pair[st->prev_byte][b]++;
            st->t1_pairs += (uint64_t)st->prev_byte * (uint64_t)b;
        }
        st->has_prev = 1;
        st->prev_byte = b;

        st->mc_carry[st->mc_carry_n++] = b;
        if (st->mc_carry_n == 6) {
            uint64_t x = (uint64_t)st->mc_carry[0] * 65536u +
                         (uint64_t)st->mc_carry[1] * 256u +
                         (uint64_t)st->mc_carry[2];
            uint64_t y = (uint64_t)st->mc_carry[3] * 65536u +
                         (uint64_t)st->mc_carry[4] * 256u +
                         (uint64_t)st->mc_carry[5];
            uint64_t lim = (1u << 24) - 1u;
            st->mc_points++;
            if (x * x + y * y <= lim * lim)
                st->mc_inside++;
            st->mc_carry_n = 0;
        }
    }
}

uint64_t parc_stats_len(const parc_stats *st)
{
    return st->n;
}

parc_err parc_stats_merge(parc_stats *dst, const parc_stats *src)
{
    if (src->n == 0)
        return PARC_OK;
    if (dst->mc_carry_n != 0)
        return PARC_ERR_ARG;

    if (dst->has_prev && src->has_first) {
        dst->pair[dst->prev_byte][src->first_byte]++;
        dst->t1_pairs += (uint64_t)dst->prev_byte * (uint64_t)src->first_byte;
    }

    for (size_t i = 0; i < 256; ++i)
        dst->hist[i] += src->hist[i];
    for (size_t c = 0; c < 256; ++c)
        for (size_t b = 0; b < 256; ++b)
            dst->pair[c][b] += src->pair[c][b];

    dst->n += src->n;
    dst->sum_u += src->sum_u;
    dst->sum_u2 += src->sum_u2;
    dst->t1_pairs += src->t1_pairs;

    if (!dst->has_first) {
        dst->has_first = src->has_first;
        dst->first_byte = src->first_byte;
    }
    if (src->has_prev) {
        dst->has_prev = 1;
        dst->prev_byte = src->prev_byte;
    }

    memcpy(dst->mc_carry, src->mc_carry, sizeof(dst->mc_carry));
    dst->mc_carry_n = src->mc_carry_n;
    dst->mc_points += src->mc_points;
    dst->mc_inside += src->mc_inside;

    return PARC_OK;
}

double parc_stats_entropy_o0(const parc_stats *st)
{
    if (st->n == 0)
        return 0.0;

    double n = (double)st->n;
    double h = 0.0;
    for (size_t i = 0; i < 256; ++i) {
        if (st->hist[i] == 0)
            continue;
        double p = (double)st->hist[i] / n;
        h -= p * log2(p);
    }
    return h;
}

double parc_stats_entropy_o1(const parc_stats *st)
{
    if (st->n < 2)
        return 0.0;

    double total_pairs = (double)(st->n - 1);
    double h = 0.0;
    for (size_t c = 0; c < 256; ++c) {
        uint64_t row_sum = 0;
        for (size_t b = 0; b < 256; ++b)
            row_sum += st->pair[c][b];
        if (row_sum == 0)
            continue;
        double rn = (double)row_sum;
        for (size_t b = 0; b < 256; ++b) {
            if (st->pair[c][b] == 0)
                continue;
            double p = (double)st->pair[c][b] / rn;
            h -= (rn / total_pairs) * p * log2(p);
        }
    }
    return h;
}

double parc_stats_min_entropy(const parc_stats *st)
{
    if (st->n == 0)
        return 0.0;

    uint64_t max_count = 0;
    for (size_t i = 0; i < 256; ++i)
        if (st->hist[i] > max_count)
            max_count = st->hist[i];

    double p = (double)max_count / (double)st->n;
    return -log2(p);
}

double parc_stats_mean(const parc_stats *st)
{
    if (st->n == 0)
        return 0.0;
    return (double)st->sum_u / (double)st->n;
}

double parc_stats_chi2(const parc_stats *st, double *p_value)
{
    if (st->n == 0) {
        if (p_value != NULL)
            *p_value = 1.0;
        return 0.0;
    }

    double expected = (double)st->n / 256.0;
    double stat = 0.0;
    for (size_t i = 0; i < 256; ++i) {
        double diff = (double)st->hist[i] - expected;
        stat += diff * diff / expected;
    }

    if (p_value != NULL)
        *p_value = parc_chi2_sf(stat, 255);
    return stat;
}

double parc_stats_serial_corr(const parc_stats *st)
{
    if (st->n < 2)
        return (double)NAN;

    uint64_t t1_int = st->t1_pairs +
                       (uint64_t)st->prev_byte * (uint64_t)st->first_byte;

    double n = (double)st->n;
    double t1 = (double)t1_int;
    double t2 = (double)st->sum_u;
    t2 = t2 * t2;
    double t3 = (double)st->sum_u2;

    double num = n * t1 - t2;
    double den = n * t3 - t2;
    if (den == 0.0)
        return (double)NAN;
    return num / den;
}

double parc_stats_montecarlo_pi(const parc_stats *st)
{
    if (st->mc_points == 0)
        return (double)NAN;
    return 4.0 * (double)st->mc_inside / (double)st->mc_points;
}

/* Regularized incomplete gamma helpers (Numerical Recipes style), used to
 * implement the chi-square survival function Q(df/2, x/2). */

#define GAMMA_ITMAX 200
#define GAMMA_EPS 3e-16
#define GAMMA_FPMIN 1e-300

/* P(a,x) via the series representation; valid for x < a+1. */
static double gamma_p_series(double a, double x)
{
    if (x <= 0.0)
        return 0.0;

    double gln = lgamma(a);
    double ap = a;
    double sum = 1.0 / a;
    double del = sum;
    for (int i = 0; i < GAMMA_ITMAX; ++i) {
        ap += 1.0;
        del *= x / ap;
        sum += del;
        if (fabs(del) < fabs(sum) * GAMMA_EPS)
            break;
    }
    return sum * exp(-x + a * log(x) - gln);
}

/* Q(a,x) via the Lentz continued fraction; valid for x >= a+1. */
static double gamma_q_cf(double a, double x)
{
    double gln = lgamma(a);
    double b = x + 1.0 - a;
    double c = 1.0 / GAMMA_FPMIN;
    double d = 1.0 / b;
    double h = d;
    for (int i = 1; i <= GAMMA_ITMAX; ++i) {
        double an = -(double)i * ((double)i - a);
        b += 2.0;
        d = an * d + b;
        if (fabs(d) < GAMMA_FPMIN)
            d = GAMMA_FPMIN;
        c = b + an / c;
        if (fabs(c) < GAMMA_FPMIN)
            c = GAMMA_FPMIN;
        d = 1.0 / d;
        double delta = d * c;
        h *= delta;
        if (fabs(delta - 1.0) < GAMMA_EPS)
            break;
    }
    return exp(-x + a * log(x) - gln) * h;
}

double parc_chi2_sf(double x, unsigned df)
{
    if (x < 0.0 || df == 0)
        return (double)NAN;
    if (x == 0.0)
        return 1.0;

    double a = (double)df / 2.0;
    double xx = x / 2.0;

    if (xx < a + 1.0)
        return 1.0 - gamma_p_series(a, xx);
    return gamma_q_cf(a, xx);
}

static double window_entropy_o0(const uint8_t *data, size_t len)
{
    if (len == 0)
        return 0.0;

    uint64_t hist[256] = {0};
    for (size_t i = 0; i < len; ++i)
        hist[data[i]]++;

    double n = (double)len;
    double h = 0.0;
    for (size_t i = 0; i < 256; ++i) {
        if (hist[i] == 0)
            continue;
        double p = (double)hist[i] / n;
        h -= p * log2(p);
    }
    return h;
}

parc_err parc_entropy_profile(const uint8_t *data, size_t len, size_t window,
                              size_t stride, double *out, size_t *out_n)
{
    if (window == 0 || stride == 0)
        return PARC_ERR_ARG;

    if (len == 0) {
        *out_n = 0;
        return PARC_OK;
    }

    if (len < window) {
        out[0] = window_entropy_o0(data, len);
        *out_n = 1;
        return PARC_OK;
    }

    size_t count = 0;
    for (size_t start = 0; start + window <= len; start += stride) {
        out[count] = window_entropy_o0(data + start, window);
        count++;
    }
    *out_n = count;
    return PARC_OK;
}

#define LZP_HASH_BITS 15
#define LZP_MIN_MATCH 4
#define LZP_WINDOW 65536

static uint32_t lzp_hash(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return (v * 2654435761u) >> (32 - LZP_HASH_BITS);
}

double parc_lz_probe(const uint8_t *data, size_t len)
{
    if (len == 0)
        return 1.0;

    /* Positions stored +1 so 0 means "empty slot". */
    uint32_t *table = (uint32_t *)calloc(1u << LZP_HASH_BITS, sizeof(*table));
    if (table == NULL) /* degrade to the all-literals estimate */
        return 9.0 / 8.0;

    uint64_t bits = 0;
    size_t i = 0;
    while (i + LZP_MIN_MATCH <= len) {
        uint32_t h = lzp_hash(data + i);
        size_t cand = table[h];
        table[h] = (uint32_t)(i + 1);

        if (cand != 0 && i + 1 - cand <= LZP_WINDOW &&
            memcmp(data + (cand - 1), data + i, LZP_MIN_MATCH) == 0) {
            size_t m = LZP_MIN_MATCH;
            while (i + m < len && data[cand - 1 + m] == data[i + m])
                m++;
            bits += 33;
            i += m;
        } else {
            bits += 9;
            i++;
        }
    }
    bits += 9 * (uint64_t)(len - i); /* tail too short to match */

    free(table);
    return (double)bits / (8.0 * (double)len);
}
