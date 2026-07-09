#ifndef PARC_GEN_H
#define PARC_GEN_H

#include <stddef.h>
#include <stdint.h>

#include "parc/err.h"
#include "util/buf.h"
#include "util/rng.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Synthetic corpus generators. Every generator appends exactly len bytes to
 * out and is a pure function of (rng state, parameters): the same seed and
 * parameters must reproduce the same bytes forever — treat generator output
 * as a stability contract like the RNG itself. len == 0 appends nothing and
 * returns PARC_OK. Only allocation failures return PARC_ERR_NOMEM; bad
 * parameters return PARC_ERR_ARG. Acceptance bands for each generator's
 * measured statistics are pinned in tests/test_gen.cpp; meeting them is part
 * of the contract.
 *
 * Entropy targets below are order-0 Shannon entropy in bits/byte as measured
 * by parc_stats_entropy_o0. */

/* Uniform random bytes (rng fill). */
parc_err parc_gen_random(parc_rng *r, size_t len, parc_buf *out);

/* Bytes with order-0 entropy ~= target_bits (in [0.0, 8.0], else
 * PARC_ERR_ARG). Method: two-component mixture — emit a uniform byte with
 * probability a, else emit 0x00 — where a is solved by bisection so the
 * mixture distribution p(0) = (1-a) + a/256, p(v!=0) = a/256 has entropy
 * target_bits. Measured entropy must land within 0.05 of target at 1 MiB. */
parc_err parc_gen_entropy(parc_rng *r, double target_bits, size_t len,
                          parc_buf *out);

/* Run-length-heavy data: geometrically distributed run lengths with the
 * given mean (>= 1.0, else PARC_ERR_ARG), each run a uniformly random byte
 * value (consecutive runs may repeat a value; that's fine). */
parc_err parc_gen_runs(parc_rng *r, double mean_run, size_t len,
                       parc_buf *out);

/* English-like text: seeded synthetic vocabulary (syllable-built words),
 * Zipf-distributed word choice, sentence capitalization and punctuation,
 * lines wrapped well under 100 columns. Output bytes are printable ASCII
 * (0x20..0x7E) or '\n'. Must show real sequential structure:
 * order-1 entropy <= order-0 entropy - 0.5 at 512 KiB. */
parc_err parc_gen_text(parc_rng *r, size_t len, parc_buf *out);

/* Newline-delimited JSON log records with a fixed key set (ts, level,
 * service, msg, latency_ms, id): monotonically increasing timestamps,
 * weighted log levels, a small service-name pool, message templates with
 * random parameters, hex ids. Every complete line (between newlines) starts
 * with '{' and ends with '}'. The final record may be truncated mid-line to
 * hit len exactly. */
parc_err parc_gen_json_log(parc_rng *r, size_t len, parc_buf *out);

/* Binary record array: fixed-size records (rec_size in [8, 4096], else
 * PARC_ERR_ARG) with delta-friendly fields — an incrementing u32 counter, a
 * jittered u32 timestamp, random-walk values — laid out little-endian. The
 * final record may be truncated to hit len exactly. */
parc_err parc_gen_records(parc_rng *r, size_t rec_size, size_t len,
                          parc_buf *out);

#ifdef __cplusplus
}
#endif

#endif /* PARC_GEN_H */
