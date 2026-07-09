#include "gen.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------- */
/* parc_gen_random                                                     */
/* ------------------------------------------------------------------- */

parc_err parc_gen_random(parc_rng *r, size_t len, parc_buf *out)
{
    if (len == 0)
        return PARC_OK;

    size_t old_len = out->len;
    parc_err e = parc_buf_reserve(out, old_len + len);
    if (e != PARC_OK)
        return e;

    parc_rng_fill(r, out->data + old_len, len);
    out->len = old_len + len;
    return PARC_OK;
}

/* ------------------------------------------------------------------- */
/* parc_gen_entropy                                                    */
/* ------------------------------------------------------------------- */

/* Order-0 Shannon entropy (bits/byte) of the two-component mixture
 * p(0) = (1-a) + a/256, p(v != 0) = a/256, for a in [0, 1]. */
static double mixture_entropy(double a)
{
    double p0 = (1.0 - a) + a / 256.0;
    double h = 0.0;
    if (p0 > 0.0)
        h -= p0 * log2(p0);
    if (a > 0.0) {
        double pv = a / 256.0;
        h -= 255.0 * pv * log2(pv);
    }
    return h;
}

/* Bisection to find a in [0,1] with mixture_entropy(a) == target. The
 * function is monotone non-decreasing in a, so bisection is well-posed. */
static double solve_mixture_a(double target)
{
    if (target <= 0.0)
        return 0.0;
    if (target >= 8.0)
        return 1.0;

    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 100; ++i) {
        double mid = (lo + hi) / 2.0;
        if (mixture_entropy(mid) < target)
            lo = mid;
        else
            hi = mid;
    }
    return (lo + hi) / 2.0;
}

parc_err parc_gen_entropy(parc_rng *r, double target_bits, size_t len,
                          parc_buf *out)
{
    if (!(target_bits >= 0.0) || target_bits > 8.0)
        return PARC_ERR_ARG;
    if (len == 0)
        return PARC_OK;

    double a = solve_mixture_a(target_bits);

    size_t old_len = out->len;
    parc_err e = parc_buf_reserve(out, old_len + len);
    if (e != PARC_OK)
        return e;

    uint8_t *p = out->data + old_len;
    for (size_t i = 0; i < len; ++i) {
        double u = parc_rng_f64(r);
        if (u < a)
            p[i] = (uint8_t)parc_rng_range(r, 256);
        else
            p[i] = 0x00;
    }
    out->len = old_len + len;
    return PARC_OK;
}

/* ------------------------------------------------------------------- */
/* parc_gen_runs                                                       */
/* ------------------------------------------------------------------- */

parc_err parc_gen_runs(parc_rng *r, double mean_run, size_t len, parc_buf *out)
{
    if (!(mean_run >= 1.0))
        return PARC_ERR_ARG;
    if (len == 0)
        return PARC_OK;

    size_t old_len = out->len;
    parc_err e = parc_buf_reserve(out, old_len + len);
    if (e != PARC_OK)
        return e;

    uint8_t *p = out->data + old_len;
    double prob = 1.0 / mean_run;
    double log1mp = (prob < 1.0) ? log(1.0 - prob) : 0.0;

    size_t pos = 0;
    while (pos < len) {
        size_t run_len;
        if (prob >= 1.0) {
            run_len = 1;
        } else {
            double u = parc_rng_f64(r);
            double l = log(1.0 - u) / log1mp;
            run_len = (size_t)l + 1;
        }
        size_t remaining = len - pos;
        if (run_len > remaining)
            run_len = remaining;

        uint8_t value = (uint8_t)parc_rng_range(r, 256);
        memset(p + pos, value, run_len);
        pos += run_len;
    }
    out->len = old_len + len;
    return PARC_OK;
}

/* ------------------------------------------------------------------- */
/* parc_gen_text                                                       */
/* ------------------------------------------------------------------- */

#define TEXT_VOCAB_N 200
#define TEXT_WORD_MAX 12
#define TEXT_WRAP_COL 78

static const char TEXT_VOWELS[] = "aeiou";
static const char TEXT_CONSONANTS[] = "bcdfghjklmnpqrstvwxyz";

static size_t build_syllable_word(parc_rng *r, char *dst)
{
    size_t nsyl = 1 + (size_t)parc_rng_range(r, 3); /* 1..3 syllables */
    size_t len = 0;

    for (size_t s = 0; s < nsyl && len < TEXT_WORD_MAX - 3; ++s) {
        /* onset consonant (skip sometimes on the first syllable) */
        if (s > 0 || parc_rng_range(r, 4) != 0) {
            size_t ci = (size_t)parc_rng_range(r, sizeof(TEXT_CONSONANTS) - 1);
            dst[len++] = TEXT_CONSONANTS[ci];
        }
        size_t vi = (size_t)parc_rng_range(r, sizeof(TEXT_VOWELS) - 1);
        dst[len++] = TEXT_VOWELS[vi];
        /* optional coda consonant */
        if (parc_rng_range(r, 5) < 2) {
            size_t ci = (size_t)parc_rng_range(r, sizeof(TEXT_CONSONANTS) - 1);
            dst[len++] = TEXT_CONSONANTS[ci];
        }
    }
    if (len == 0)
        dst[len++] = 'a';
    return len;
}

parc_err parc_gen_text(parc_rng *r, size_t len, parc_buf *out)
{
    if (len == 0)
        return PARC_OK;

    /* Build a deterministic vocabulary and Zipf-like weights. */
    char words[TEXT_VOCAB_N][TEXT_WORD_MAX];
    uint8_t wlen[TEXT_VOCAB_N];
    double cum[TEXT_VOCAB_N];
    double total = 0.0;

    for (size_t i = 0; i < TEXT_VOCAB_N; ++i) {
        wlen[i] = (uint8_t)build_syllable_word(r, words[i]);
        double weight = 1.0 / pow((double)(i + 1), 1.1);
        total += weight;
        cum[i] = total;
    }

    size_t old_len = out->len;
    parc_err e = parc_buf_reserve(out, old_len + len);
    if (e != PARC_OK)
        return e;

    uint8_t *p = out->data + old_len;
    size_t pos = 0;
    size_t col = 0;
    int need_cap = 1;

    while (pos < len) {
        size_t sent_words = 4 + (size_t)parc_rng_range(r, 9); /* 4..12 */

        for (size_t w = 0; w < sent_words && pos < len; ++w) {
            double u = parc_rng_f64(r) * total;
            size_t idx = 0;
            while (idx < TEXT_VOCAB_N - 1 && cum[idx] < u)
                idx++;

            const char *word = words[idx];
            size_t wl = wlen[idx];

            if (col > 0) {
                size_t need = 1 + wl;
                if (col + need > TEXT_WRAP_COL) {
                    p[pos++] = '\n';
                    col = 0;
                    if (pos >= len)
                        break;
                } else {
                    p[pos++] = ' ';
                    col++;
                    if (pos >= len)
                        break;
                }
            }

            for (size_t k = 0; k < wl && pos < len; ++k) {
                char c = word[k];
                if (k == 0 && need_cap)
                    c = (char)(c - 'a' + 'A');
                p[pos++] = (uint8_t)c;
                col++;
            }
            need_cap = 0;
        }

        if (pos < len) {
            p[pos++] = '.';
            col++;
            need_cap = 1;
        }
    }

    out->len = old_len + len;
    return PARC_OK;
}

/* ------------------------------------------------------------------- */
/* parc_gen_json_log                                                   */
/* ------------------------------------------------------------------- */

static const char *const JSON_LEVELS[] = {"INFO", "INFO", "INFO", "DEBUG",
                                           "WARN", "ERROR"};
#define JSON_LEVELS_N (sizeof(JSON_LEVELS) / sizeof(JSON_LEVELS[0]))

static const char *const JSON_SERVICES[] = {
    "api-gateway", "auth-svc", "billing-svc", "search-svc", "worker-01"};
#define JSON_SERVICES_N (sizeof(JSON_SERVICES) / sizeof(JSON_SERVICES[0]))

static const char *const JSON_MSGS[] = {
    "request completed",      "request failed",     "cache miss",
    "cache hit",               "connection reset",    "retrying operation",
    "timeout waiting for peer", "queue drained",       "rate limit exceeded",
    "health check ok"};
#define JSON_MSGS_N (sizeof(JSON_MSGS) / sizeof(JSON_MSGS[0]))

static const char HEX_DIGITS[] = "0123456789abcdef";

parc_err parc_gen_json_log(parc_rng *r, size_t len, parc_buf *out)
{
    if (len == 0)
        return PARC_OK;

    size_t old_len = out->len;
    parc_err e = parc_buf_reserve(out, old_len + len);
    if (e != PARC_OK)
        return e;

    uint8_t *p = out->data + old_len;
    size_t pos = 0;

    uint64_t ts = 1700000000000ULL;

    char line[256];

    while (pos < len) {
        ts += 1 + parc_rng_range(r, 500);

        const char *level = JSON_LEVELS[parc_rng_range(r, JSON_LEVELS_N)];
        const char *service = JSON_SERVICES[parc_rng_range(r, JSON_SERVICES_N)];
        const char *msg = JSON_MSGS[parc_rng_range(r, JSON_MSGS_N)];
        unsigned latency = (unsigned)parc_rng_range(r, 2000);

        char id[9];
        for (int i = 0; i < 8; ++i)
            id[i] = HEX_DIGITS[parc_rng_range(r, 16)];
        id[8] = '\0';

        int n = snprintf(line, sizeof(line),
                          "{\"ts\":%llu,\"level\":\"%s\",\"service\":\"%s\","
                          "\"msg\":\"%s\",\"latency_ms\":%u,\"id\":\"%s\"}\n",
                          (unsigned long long)ts, level, service, msg, latency,
                          id);
        if (n < 0)
            continue;

        size_t line_len = (size_t)n;
        size_t remaining = len - pos;
        size_t copy_len = (line_len < remaining) ? line_len : remaining;
        memcpy(p + pos, line, copy_len);
        pos += copy_len;
    }

    out->len = old_len + len;
    return PARC_OK;
}

/* ------------------------------------------------------------------- */
/* parc_gen_records                                                    */
/* ------------------------------------------------------------------- */

#define RECORDS_MAX_WALK_CHANNELS 1022

static void put_u32le(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
    dst[2] = (uint8_t)((v >> 16) & 0xFFu);
    dst[3] = (uint8_t)((v >> 24) & 0xFFu);
}

parc_err parc_gen_records(parc_rng *r, size_t rec_size, size_t len,
                          parc_buf *out)
{
    if (rec_size < 8 || rec_size > 4096)
        return PARC_ERR_ARG;
    if (len == 0)
        return PARC_OK;

    size_t old_len = out->len;
    parc_err e = parc_buf_reserve(out, old_len + len);
    if (e != PARC_OK)
        return e;

    uint8_t *out_p = out->data + old_len;

    size_t tail = rec_size - 8;
    size_t nchan = tail / 4;
    if (nchan > RECORDS_MAX_WALK_CHANNELS)
        nchan = RECORDS_MAX_WALK_CHANNELS;
    size_t pad = tail - nchan * 4;

    uint32_t walk[RECORDS_MAX_WALK_CHANNELS];
    memset(walk, 0, sizeof(walk));

    uint32_t base_ts = 1000000u;

    uint8_t rec[4096];
    memset(rec, 0, sizeof(rec));

    uint32_t counter = 0;
    size_t pos = 0;

    while (pos < len) {
        put_u32le(rec, counter);

        uint32_t jitter = (uint32_t)parc_rng_range(r, 1000);
        uint32_t ts = base_ts + counter + jitter;
        put_u32le(rec + 4, ts);

        size_t off = 8;
        for (size_t c = 0; c < nchan; ++c) {
            int32_t delta = (int32_t)parc_rng_range(r, 201) - 100;
            walk[c] = (uint32_t)((int32_t)walk[c] + delta);
            put_u32le(rec + off, walk[c]);
            off += 4;
        }
        for (size_t k = 0; k < pad; ++k)
            rec[off + k] = 0;

        size_t remaining = len - pos;
        size_t copy_len = (rec_size < remaining) ? rec_size : remaining;
        memcpy(out_p + pos, rec, copy_len);
        pos += copy_len;
        counter++;
    }

    out->len = old_len + len;
    return PARC_OK;
}
