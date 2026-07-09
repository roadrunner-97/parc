#ifndef PARC_STAT_STATS_H
#define PARC_STAT_STATS_H

#include <stddef.h>
#include <stdint.h>

#include "parc/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Streaming statistical analyzer — the core of parcent and, later, the
 * codec's per-block entropy probe. Feed bytes in arbitrary chunks; results
 * are readable at any point without disturbing the stream. All results must
 * be exactly independent of how the input was chunked (order-1 pairs and
 * Monte Carlo 6-byte groups span update boundaries).
 *
 * The accumulator owns a 256x256 pair table (~512 KiB), so instances are
 * heap-allocated via create/destroy. */

typedef struct parc_stats parc_stats;

parc_err parc_stats_create(parc_stats **out);
void parc_stats_destroy(parc_stats *st);
void parc_stats_reset(parc_stats *st);
void parc_stats_update(parc_stats *st, const void *data, size_t len);

/* Total bytes consumed. */
uint64_t parc_stats_len(const parc_stats *st);

/* Fold src into dst as if src's bytes had been fed to dst immediately after
 * dst's own — the boundary pair (dst's last byte, src's first byte) is
 * counted. Every readable result on the merged dst must equal what a single
 * accumulator fed the concatenated stream would report.
 *
 * Because Monte Carlo groups are positional and the raw bytes are gone,
 * dst's length must be a multiple of 6 (no partial group pending) unless
 * src is empty; otherwise PARC_ERR_ARG and dst is unchanged. src is not
 * modified. dst and src must be distinct. */
parc_err parc_stats_merge(parc_stats *dst, const parc_stats *src);

/* Order-0 Shannon entropy in bits/byte: -sum p_i log2 p_i over the byte
 * histogram. Empty input -> 0.0. */
double parc_stats_entropy_o0(const parc_stats *st);

/* Order-1 conditional entropy H(X_n | X_{n-1}) in bits/byte, computed from
 * the (previous byte, next byte) pair counts: sum over contexts c of
 * (count_c / total_pairs) * H(next | c). Fewer than 2 bytes -> 0.0. */
double parc_stats_entropy_o1(const parc_stats *st);

/* Min-entropy in bits/byte: -log2(max_i p_i). Empty input -> 0.0. */
double parc_stats_min_entropy(const parc_stats *st);

/* Arithmetic mean of byte values. Empty input -> 0.0. */
double parc_stats_mean(const parc_stats *st);

/* Chi-square statistic of the byte histogram against the uniform
 * distribution (expected n/256 per cell, df = 255). If p_value is non-NULL,
 * *p_value = parc_chi2_sf(statistic, 255). Empty input -> 0.0, p = 1.0. */
double parc_stats_chi2(const parc_stats *st, double *p_value);

/* Lag-1 serial correlation coefficient, ent-compatible (circular: the final
 * term multiplies the last byte by the first):
 *   t1 = sum_{i=0}^{n-2} u_i*u_{i+1} + u_{n-1}*u_0
 *   t2 = (sum u_i)^2,  t3 = sum u_i^2
 *   scc = (n*t1 - t2) / (n*t3 - t2)
 * Returns NaN when the denominator is 0 (e.g. constant input) or n < 2. */
double parc_stats_serial_corr(const parc_stats *st);

/* Monte Carlo pi, ent-compatible: consecutive non-overlapping 6-byte groups
 * form x = b0*65536 + b1*256 + b2 and y = b3*65536 + b4*256 + b5; a point is
 * inside if x*x + y*y <= (2^24 - 1)^2 (use unsigned 64-bit or double for the
 * squares). Returns 4.0 * inside / points; NaN if no complete group yet.
 * Trailing partial groups are carried across updates, ignored at read. */
double parc_stats_montecarlo_pi(const parc_stats *st);

/* Upper-tail probability (survival function) of the chi-square distribution:
 * Q(df/2, x/2), the regularized upper incomplete gamma function. Implement
 * with the standard series (x < df+1) / Lentz continued fraction (otherwise)
 * pair using lgamma(); relative error < 1e-9 over the tested range.
 * x < 0 or df == 0 -> NaN. x == 0 -> 1.0. Reference values from scipy are
 * pinned in tests/test_stats.cpp. */
double parc_chi2_sf(double x, unsigned df);

/* Sliding-window order-0 entropy profile over a complete buffer (not
 * streaming). Windows start at offsets 0, stride, 2*stride, ... and only
 * complete windows are emitted, except: 0 < len < window emits one window
 * covering all of data. out must have room for the emitted count; *out_n is
 * set to it. window == 0 or stride == 0 -> PARC_ERR_ARG. len == 0 -> 0
 * windows, PARC_OK. */
parc_err parc_entropy_profile(const uint8_t *data, size_t len, size_t window,
                              size_t stride, double *out, size_t *out_n);

/* Fast LZ-compressibility probe over a complete buffer (not streaming).
 * Greedy single-entry hash-table match search (4-byte minimum match,
 * 64 KiB window) charged with a fixed cost model: 9 bits per literal byte,
 * 33 bits per match regardless of length. Returns estimated compressed size
 * over input size — lower means more LZ-compressible. Incompressible input
 * approaches 9/8 = 1.125; len == 0 returns 1.0. Deterministic: a pure
 * function of the buffer contents. This is an ordering signal for corpus
 * tagging and the codec's stored-block bailout, not a promise of achievable
 * ratio. */
double parc_lz_probe(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* PARC_STAT_STATS_H */
