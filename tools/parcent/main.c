/* parcent — entropy and randomness analyzer (the `ent` replacement).
 *
 * Regular files are mmap'd and analyzed with worker threads over 6-byte-
 * aligned partitions (parc_stats_merge reconstitutes exact single-pass
 * results); stdin and pipes are streamed single-threaded, where the
 * buffer-based measures (LZ probe, entropy profile) are unavailable.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "stat/stats.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define STREAM_CHUNK (1u << 20)
#define MIN_BYTES_PER_THREAD (1u << 20)

enum out_mode { OUT_TEXT, OUT_JSON, OUT_CSV };

struct options {
    enum out_mode mode;
    size_t profile_window; /* 0 = no profile */
    size_t profile_stride;
    unsigned threads;
};

struct report {
    const char *name;
    uint64_t bytes;
    double o0, o1, min_entropy, mean;
    double chi2, chi2_p;
    double scc, mc_pi;
    double lz;         /* NaN when input was streamed */
    double *profile;   /* NULL unless requested and input was mapped */
    size_t profile_n;
};

static void usage(FILE *to)
{
    fputs(
        "usage: parcent [options] [FILE...]\n"
        "Analyze byte-level statistics of FILEs (stdin if none or \"-\").\n"
        "\n"
        "  -j, --json            JSON output (array of objects)\n"
        "  -c, --csv             CSV output\n"
        "  -p, --profile W[,S]   sliding-window entropy profile: window W\n"
        "                        bytes, stride S (default W); sizes accept\n"
        "                        K/M/G suffixes\n"
        "  -t, --threads N       worker threads for regular files\n"
        "                        (default: online CPUs)\n"
        "  -h, --help            show this help\n"
        "\n"
        "Streamed inputs (stdin, pipes) omit the LZ probe and profile.\n",
        to);
}

static int parse_size(const char *s, size_t *out)
{
    if (*s < '0' || *s > '9') /* strtoull would accept "-5" by wrapping */
        return -1;
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

/* ---- analysis ---------------------------------------------------------- */

struct worker {
    pthread_t tid;
    const uint8_t *data;
    size_t len;
    parc_stats *st;
    parc_err err;
};

static void *worker_main(void *arg)
{
    struct worker *w = (struct worker *)arg;
    w->err = parc_stats_create(&w->st);
    if (w->err == PARC_OK)
        parc_stats_update(w->st, w->data, w->len);
    return NULL;
}

static parc_err stats_over_buffer(const uint8_t *data, size_t len,
                                  unsigned threads, parc_stats *st)
{
    size_t want = len / MIN_BYTES_PER_THREAD;
    unsigned t = threads;
    if (want < t)
        t = want > 0 ? (unsigned)want : 1;

    if (t <= 1) {
        parc_stats_update(st, data, len);
        return PARC_OK;
    }

    struct worker *ws = calloc(t, sizeof(*ws));
    if (ws == NULL)
        return PARC_ERR_NOMEM;

    /* Chunk sizes are multiples of 6, so every chunk starts on a Monte
     * Carlo group boundary and merge preconditions hold; the last chunk
     * takes the remainder. */
    size_t chunk = (len / t) - (len / t) % 6;
    size_t off = 0;
    for (unsigned i = 0; i < t; ++i) {
        ws[i].data = data + off;
        ws[i].len = i + 1 < t ? chunk : len - off;
        off += ws[i].len;
        if (pthread_create(&ws[i].tid, NULL, worker_main, &ws[i]) != 0) {
            /* fall back: run remaining chunks inline */
            worker_main(&ws[i]);
            ws[i].tid = 0;
        }
    }

    parc_err err = PARC_OK;
    for (unsigned i = 0; i < t; ++i) {
        if (ws[i].tid != 0)
            pthread_join(ws[i].tid, NULL);
        if (err == PARC_OK)
            err = ws[i].err;
        if (err == PARC_OK)
            err = parc_stats_merge(st, ws[i].st);
        parc_stats_destroy(ws[i].st);
    }
    free(ws);
    return err;
}

static int analyze_mapped(const char *name, const uint8_t *data, size_t len,
                          const struct options *opt, parc_stats *st,
                          struct report *r)
{
    parc_err err = stats_over_buffer(data, len, opt->threads, st);
    if (err != PARC_OK) {
        fprintf(stderr, "parcent: %s: %s\n", name, parc_err_str(err));
        return -1;
    }

    r->lz = parc_lz_probe(data, len);

    if (opt->profile_window > 0 && len > 0) {
        size_t w = opt->profile_window, s = opt->profile_stride;
        size_t cap = len < w ? 1 : (len - w) / s + 1;
        r->profile = malloc(cap * sizeof(*r->profile));
        if (r->profile == NULL) {
            fprintf(stderr, "parcent: %s: %s\n", name,
                    parc_err_str(PARC_ERR_NOMEM));
            return -1;
        }
        err = parc_entropy_profile(data, len, w, s, r->profile,
                                   &r->profile_n);
        if (err != PARC_OK) {
            fprintf(stderr, "parcent: %s: %s\n", name, parc_err_str(err));
            return -1;
        }
    }
    return 0;
}

static int analyze_stream(const char *name, FILE *f, parc_stats *st)
{
    uint8_t *buf = malloc(STREAM_CHUNK);
    if (buf == NULL) {
        fprintf(stderr, "parcent: %s: %s\n", name,
                parc_err_str(PARC_ERR_NOMEM));
        return -1;
    }
    for (;;) {
        size_t got = fread(buf, 1, STREAM_CHUNK, f);
        if (got > 0)
            parc_stats_update(st, buf, got);
        if (got < STREAM_CHUNK) {
            int bad = ferror(f);
            free(buf);
            if (bad) {
                fprintf(stderr, "parcent: %s: read error\n", name);
                return -1;
            }
            return 0;
        }
    }
}

static void fill_report(const parc_stats *st, struct report *r)
{
    r->bytes = parc_stats_len(st);
    r->o0 = parc_stats_entropy_o0(st);
    r->o1 = parc_stats_entropy_o1(st);
    r->min_entropy = parc_stats_min_entropy(st);
    r->mean = parc_stats_mean(st);
    r->chi2 = parc_stats_chi2(st, &r->chi2_p);
    r->scc = parc_stats_serial_corr(st);
    r->mc_pi = parc_stats_montecarlo_pi(st);
}

static int analyze(const char *path, const struct options *opt,
                   struct report *r)
{
    memset(r, 0, sizeof(*r));
    r->name = strcmp(path, "-") == 0 ? "(stdin)" : path;
    r->lz = (double)NAN;

    parc_stats *st = NULL;
    if (parc_stats_create(&st) != PARC_OK) {
        fprintf(stderr, "parcent: %s\n", parc_err_str(PARC_ERR_NOMEM));
        return -1;
    }

    int rc;
    if (strcmp(path, "-") == 0) {
        rc = analyze_stream(r->name, stdin, st);
    } else {
        int fd = open(path, O_RDONLY);
        struct stat sb;
        if (fd < 0 || fstat(fd, &sb) != 0) {
            fprintf(stderr, "parcent: %s: %s\n", path, strerror(errno));
            if (fd >= 0)
                close(fd);
            parc_stats_destroy(st);
            return -1;
        }
        if (S_ISREG(sb.st_mode) && sb.st_size > 0) {
            size_t len = (size_t)sb.st_size;
            void *map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
            if (map == MAP_FAILED) {
                fprintf(stderr, "parcent: %s: mmap: %s\n", path,
                        strerror(errno));
                close(fd);
                parc_stats_destroy(st);
                return -1;
            }
            rc = analyze_mapped(path, map, len, opt, st, r);
            munmap(map, len);
        } else if (S_ISREG(sb.st_mode)) {
            rc = 0; /* empty regular file */
            r->lz = 1.0;
        } else {
            FILE *f = fdopen(fd, "rb");
            if (f == NULL) {
                fprintf(stderr, "parcent: %s: %s\n", path, strerror(errno));
                close(fd);
                parc_stats_destroy(st);
                return -1;
            }
            rc = analyze_stream(path, f, st);
            fclose(f);
            fd = -1;
        }
        if (fd >= 0)
            close(fd);
    }

    if (rc == 0)
        fill_report(st, r);
    parc_stats_destroy(st);
    return rc;
}

/* ---- output ------------------------------------------------------------ */

static void print_text(const struct report *r, const struct options *opt)
{
    printf("%s:\n", r->name);
    printf("  bytes                 %llu\n", (unsigned long long)r->bytes);
    printf("  entropy (order-0)     %.6f bits/byte\n", r->o0);
    printf("  entropy (order-1)     %.6f bits/byte\n", r->o1);
    printf("  min-entropy           %.6f bits/byte\n", r->min_entropy);
    printf("  optimum compression   would reduce by %.2f%%\n",
           (8.0 - r->o0) / 8.0 * 100.0);
    printf("  chi-square            %.2f (p = %.4f)\n", r->chi2, r->chi2_p);
    printf("  arithmetic mean       %.4f (127.5 = random)\n", r->mean);
    if (isnan(r->mc_pi))
        printf("  monte carlo pi        (need >= 6 bytes)\n");
    else
        printf("  monte carlo pi        %.9f (error %.2f%%)\n", r->mc_pi,
               fabs(r->mc_pi - M_PI) / M_PI * 100.0);
    if (isnan(r->scc))
        printf("  serial correlation    undefined\n");
    else
        printf("  serial correlation    %.6f (0.0 = uncorrelated)\n", r->scc);
    if (!isnan(r->lz))
        printf("  lz probe ratio        %.4f (lower = more compressible)\n",
               r->lz);
    if (opt->profile_window > 0 && r->profile_n > 0) {
        double lo = r->profile[0], hi = r->profile[0], sum = 0.0;
        for (size_t i = 0; i < r->profile_n; ++i) {
            if (r->profile[i] < lo)
                lo = r->profile[i];
            if (r->profile[i] > hi)
                hi = r->profile[i];
            sum += r->profile[i];
        }
        printf("  entropy profile       %zu windows of %zu: "
               "min %.4f / mean %.4f / max %.4f\n",
               r->profile_n, opt->profile_window, lo,
               sum / (double)r->profile_n, hi);
    }
}

/* JSON numbers can't be NaN; emit null. */
static void json_num(const char *key, double v, const char *trail)
{
    if (isnan(v))
        printf("    \"%s\": null%s\n", key, trail);
    else
        printf("    \"%s\": %.12g%s\n", key, v, trail);
}

static void json_escaped(const char *s)
{
    putchar('"');
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\')
            printf("\\%c", *s);
        else if ((unsigned char)*s < 0x20)
            printf("\\u%04x", (unsigned)(unsigned char)*s);
        else
            putchar(*s);
    }
    putchar('"');
}

static void print_json(const struct report *r, const struct options *opt,
                       int first)
{
    if (!first)
        puts("  ,");
    printf("  {\n    \"file\": ");
    json_escaped(r->name);
    printf(",\n");
    printf("    \"bytes\": %llu,\n", (unsigned long long)r->bytes);
    json_num("entropy_o0", r->o0, ",");
    json_num("entropy_o1", r->o1, ",");
    json_num("min_entropy", r->min_entropy, ",");
    json_num("mean", r->mean, ",");
    json_num("chi2", r->chi2, ",");
    json_num("chi2_p", r->chi2_p, ",");
    json_num("serial_corr", r->scc, ",");
    json_num("montecarlo_pi", r->mc_pi, ",");
    if (opt->profile_window > 0 && r->profile != NULL) {
        json_num("lz_probe", r->lz, ",");
        printf("    \"profile\": { \"window\": %zu, \"stride\": %zu, "
               "\"entropy\": [",
               opt->profile_window, opt->profile_stride);
        for (size_t i = 0; i < r->profile_n; ++i)
            printf("%s%.6g", i > 0 ? ", " : "", r->profile[i]);
        printf("] }\n");
    } else {
        json_num("lz_probe", r->lz, "");
    }
    puts("  }");
}

/* RFC 4180 quoting for names containing separators or quotes. */
static void csv_name(const char *s)
{
    if (strpbrk(s, ",\"\n\r") == NULL) {
        fputs(s, stdout);
        return;
    }
    putchar('"');
    for (; *s; ++s) {
        if (*s == '"')
            putchar('"');
        putchar(*s);
    }
    putchar('"');
}

static void csv_num(double v, const char *trail)
{
    if (isnan(v))
        printf("%s", trail);
    else
        printf("%.12g%s", v, trail);
}

static void print_csv_header(const struct options *opt)
{
    if (opt->profile_window > 0)
        puts("file,offset,entropy");
    else
        puts("file,bytes,entropy_o0,entropy_o1,min_entropy,mean,"
             "chi2,chi2_p,serial_corr,montecarlo_pi,lz_probe");
}

static void print_csv(const struct report *r, const struct options *opt)
{
    if (opt->profile_window > 0) {
        /* one row per window, for plotting */
        for (size_t i = 0; i < r->profile_n; ++i) {
            csv_name(r->name);
            printf(",%zu,%.6g\n", i * opt->profile_stride, r->profile[i]);
        }
        return;
    }
    csv_name(r->name);
    printf(",%llu,", (unsigned long long)r->bytes);
    csv_num(r->o0, ",");
    csv_num(r->o1, ",");
    csv_num(r->min_entropy, ",");
    csv_num(r->mean, ",");
    csv_num(r->chi2, ",");
    csv_num(r->chi2_p, ",");
    csv_num(r->scc, ",");
    csv_num(r->mc_pi, ",");
    csv_num(r->lz, "\n");
}

/* ---- main -------------------------------------------------------------- */

static int parse_profile_arg(const char *arg, struct options *opt)
{
    char tmp[64];
    if (strlen(arg) >= sizeof(tmp))
        return -1;
    strcpy(tmp, arg);
    char *comma = strchr(tmp, ',');
    if (comma != NULL)
        *comma = '\0';
    if (parse_size(tmp, &opt->profile_window) != 0 ||
        opt->profile_window == 0)
        return -1;
    if (comma != NULL) {
        if (parse_size(comma + 1, &opt->profile_stride) != 0 ||
            opt->profile_stride == 0)
            return -1;
    } else {
        opt->profile_stride = opt->profile_window;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct options opt = {0};
    opt.mode = OUT_TEXT;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    opt.threads = ncpu > 0 ? (unsigned)ncpu : 1;

    int argi = 1;
    for (; argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0';
         ++argi) {
        const char *a = argv[argi];
        if (strcmp(a, "-j") == 0 || strcmp(a, "--json") == 0) {
            opt.mode = OUT_JSON;
        } else if (strcmp(a, "-c") == 0 || strcmp(a, "--csv") == 0) {
            opt.mode = OUT_CSV;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(a, "-p") == 0 || strcmp(a, "--profile") == 0) {
            if (argi + 1 >= argc ||
                parse_profile_arg(argv[++argi], &opt) != 0) {
                fprintf(stderr, "parcent: bad or missing profile spec\n");
                return 2;
            }
        } else if (strcmp(a, "-t") == 0 || strcmp(a, "--threads") == 0) {
            size_t t = 0;
            if (argi + 1 >= argc || parse_size(argv[++argi], &t) != 0 ||
                t == 0 || t > 4096) {
                fprintf(stderr, "parcent: bad or missing thread count\n");
                return 2;
            }
            opt.threads = (unsigned)t;
        } else if (strcmp(a, "--") == 0) {
            ++argi;
            break;
        } else {
            fprintf(stderr, "parcent: unknown option %s\n", a);
            usage(stderr);
            return 2;
        }
    }

    static const char *const dash = "-";
    const char *const *files;
    int nfiles;
    if (argi < argc) {
        files = (const char *const *)&argv[argi];
        nfiles = argc - argi;
    } else {
        files = &dash;
        nfiles = 1;
    }

    int failures = 0;
    int emitted = 0;
    if (opt.mode == OUT_JSON)
        puts("[");
    else if (opt.mode == OUT_CSV)
        print_csv_header(&opt);

    for (int i = 0; i < nfiles; ++i) {
        struct report r;
        if (analyze(files[i], &opt, &r) != 0) {
            failures++;
            free(r.profile);
            continue;
        }
        switch (opt.mode) {
        case OUT_TEXT:
            if (emitted > 0)
                putchar('\n');
            print_text(&r, &opt);
            break;
        case OUT_JSON:
            print_json(&r, &opt, emitted == 0);
            break;
        case OUT_CSV:
            print_csv(&r, &opt);
            break;
        }
        emitted++;
        free(r.profile);
    }

    if (opt.mode == OUT_JSON)
        puts("]");

    return failures > 0 ? 1 : 0;
}
