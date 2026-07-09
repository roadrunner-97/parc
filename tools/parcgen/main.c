/* parcgen — seeded synthetic corpus generator CLI over the parc_gen_*
 * generators. Same seed + arguments reproduce the same bytes forever. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen.h"
#include "util/buf.h"
#include "util/rng.h"

static void usage(FILE *to)
{
    fputs(
        "usage: parcgen [options] CLASS\n"
        "Generate synthetic corpus data (stdout unless -o).\n"
        "\n"
        "classes:\n"
        "  random             uniform random bytes (8.0 bits/byte)\n"
        "  entropy BITS       order-0 entropy ~= BITS (0.0 .. 8.0)\n"
        "  runs MEAN          runs with geometric mean length MEAN (>= 1)\n"
        "  text               English-like Zipf text\n"
        "  json-log           newline-delimited JSON log records\n"
        "  records SIZE       binary records of SIZE bytes (8 .. 4096)\n"
        "\n"
        "options:\n"
        "  -s, --seed N       RNG seed (default 0)\n"
        "  -n, --size BYTES   output length; K/M/G suffixes ok (default 1M)\n"
        "  -o, --out FILE     write to FILE instead of stdout\n"
        "  -h, --help         show this help\n",
        to);
}

static int parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0')
        return -1;
    *out = v;
    return 0;
}

static int parse_size(const char *s, size_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || end == s)
        return -1;
    unsigned long long mult = 1;
    if (*end == 'K' || *end == 'k')
        mult = 1ull << 10;
    else if (*end == 'M' || *end == 'm')
        mult = 1ull << 20;
    else if (*end == 'G' || *end == 'g')
        mult = 1ull << 30;
    if (mult > 1)
        end++;
    if (*end != '\0' || v > SIZE_MAX / mult)
        return -1;
    *out = (size_t)(v * mult);
    return 0;
}

static int parse_f64(const char *s, double *out)
{
    char *end = NULL;
    errno = 0;
    double v = strtod(s, &end);
    if (errno != 0 || end == s || *end != '\0')
        return -1;
    *out = v;
    return 0;
}

int main(int argc, char **argv)
{
    uint64_t seed = 0;
    size_t size = 1u << 20;
    const char *out_path = NULL;

    int argi = 1;
    for (; argi < argc && argv[argi][0] == '-'; ++argi) {
        const char *a = argv[argi];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(a, "-s") == 0 || strcmp(a, "--seed") == 0) {
            if (argi + 1 >= argc || parse_u64(argv[++argi], &seed) != 0) {
                fprintf(stderr, "parcgen: bad or missing seed\n");
                return 2;
            }
        } else if (strcmp(a, "-n") == 0 || strcmp(a, "--size") == 0) {
            if (argi + 1 >= argc || parse_size(argv[++argi], &size) != 0) {
                fprintf(stderr, "parcgen: bad or missing size\n");
                return 2;
            }
        } else if (strcmp(a, "-o") == 0 || strcmp(a, "--out") == 0) {
            if (argi + 1 >= argc) {
                fprintf(stderr, "parcgen: missing output path\n");
                return 2;
            }
            out_path = argv[++argi];
        } else if (strcmp(a, "--") == 0) {
            ++argi;
            break;
        } else {
            fprintf(stderr, "parcgen: unknown option %s\n", a);
            usage(stderr);
            return 2;
        }
    }

    if (argi >= argc) {
        fprintf(stderr, "parcgen: missing generator class\n");
        usage(stderr);
        return 2;
    }
    const char *cls = argv[argi++];
    const char *param = argi < argc ? argv[argi++] : NULL;
    if (argi < argc) {
        fprintf(stderr, "parcgen: unexpected argument %s\n", argv[argi]);
        return 2;
    }

    parc_rng rng;
    parc_rng_seed(&rng, seed);
    parc_buf buf;
    parc_buf_init(&buf);

    parc_err err;
    if (strcmp(cls, "random") == 0 && param == NULL) {
        err = parc_gen_random(&rng, size, &buf);
    } else if (strcmp(cls, "entropy") == 0 && param != NULL) {
        double bits;
        if (parse_f64(param, &bits) != 0) {
            fprintf(stderr, "parcgen: bad entropy target %s\n", param);
            return 2;
        }
        err = parc_gen_entropy(&rng, bits, size, &buf);
    } else if (strcmp(cls, "runs") == 0 && param != NULL) {
        double mean;
        if (parse_f64(param, &mean) != 0) {
            fprintf(stderr, "parcgen: bad run mean %s\n", param);
            return 2;
        }
        err = parc_gen_runs(&rng, mean, size, &buf);
    } else if (strcmp(cls, "text") == 0 && param == NULL) {
        err = parc_gen_text(&rng, size, &buf);
    } else if (strcmp(cls, "json-log") == 0 && param == NULL) {
        err = parc_gen_json_log(&rng, size, &buf);
    } else if (strcmp(cls, "records") == 0 && param != NULL) {
        size_t rec = 0;
        if (parse_size(param, &rec) != 0) {
            fprintf(stderr, "parcgen: bad record size %s\n", param);
            return 2;
        }
        err = parc_gen_records(&rng, rec, size, &buf);
    } else {
        fprintf(stderr, "parcgen: unknown class or bad arguments: %s\n", cls);
        usage(stderr);
        return 2;
    }

    if (err != PARC_OK) {
        fprintf(stderr, "parcgen: %s\n", parc_err_str(err));
        parc_buf_free(&buf);
        return 1;
    }

    FILE *out = stdout;
    if (out_path != NULL) {
        out = fopen(out_path, "wb");
        if (out == NULL) {
            fprintf(stderr, "parcgen: %s: %s\n", out_path, strerror(errno));
            parc_buf_free(&buf);
            return 1;
        }
    }

    int rc = 0;
    if (buf.len > 0 && fwrite(buf.data, 1, buf.len, out) != buf.len) {
        fprintf(stderr, "parcgen: write failed\n");
        rc = 1;
    }
    if (out != stdout && fclose(out) != 0) {
        fprintf(stderr, "parcgen: %s: %s\n", out_path, strerror(errno));
        rc = 1;
    }
    parc_buf_free(&buf);
    return rc;
}
