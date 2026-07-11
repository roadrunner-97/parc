/* parc — CLI over the parc codec: compress, decompress, verify.
 * Format v0 per docs/FORMAT.md; files or stdin/stdout. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "parc/parc.h"

static void usage(FILE *to)
{
    fputs(
        "usage: parc COMMAND [options] [FILE]\n"
        "Compress or decompress a single parc frame (stdin/stdout by "
        "default).\n"
        "\n"
        "commands:\n"
        "  c, compress        compress FILE (or stdin) to -o (or stdout)\n"
        "  d, decompress      decompress FILE (or stdin) to -o (or stdout)\n"
        "  t, verify          decode FILE (or stdin), checking every hash,\n"
        "                     writing nothing\n"
        "\n"
        "options:\n"
        "  -b, --block-log N  compress with max block size 2^N bytes,\n"
        "                     N in 12..24 (default 20 = 1 MiB)\n"
        "  -L, --level N      compression level N in 1..9 (default 3);\n"
        "                     higher is smaller but slower\n"
        "  -T, --threads N    use N worker threads (0 = one per CPU;\n"
        "                     default 1)\n"
        "  -o, --out FILE     write to FILE instead of stdout\n"
        "  -v, --verbose      print a frame summary to stderr\n"
        "  -h, --help         show this help\n",
        to);
}

static int parse_uint(const char *s, unsigned *out)
{
    if (*s < '0' || *s > '9')
        return -1;
    char *end = NULL;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v > UINT32_MAX)
        return -1;
    *out = (unsigned)v;
    return 0;
}

static void report(FILE *to, const char *verb, const parc_info *fi)
{
    double ratio = fi->raw_bytes
                       ? (double)fi->frame_bytes / (double)fi->raw_bytes
                       : 0.0;
    fprintf(to,
            "parc: %s %llu -> %llu bytes (ratio %.4f), %u blocks "
            "(%u stored)\n",
            verb, (unsigned long long)fi->raw_bytes,
            (unsigned long long)fi->frame_bytes, ratio, fi->blocks,
            fi->stored_blocks);
}

int main(int argc, char **argv)
{
    enum { CMD_NONE, CMD_C, CMD_D, CMD_T } cmd = CMD_NONE;
    const char *in_path = NULL, *out_path = NULL;
    parc_copts copts = {0, 0, 0};
    unsigned threads = 1;
    int verbose = 0;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(a, "-b") == 0 || strcmp(a, "--block-log") == 0) {
            if (++i >= argc || parse_uint(argv[i], &copts.block_log) != 0 ||
                copts.block_log < 12 || copts.block_log > 24) {
                fputs("parc: -b needs an integer in 12..24\n", stderr);
                return 2;
            }
        } else if (strcmp(a, "-L") == 0 || strcmp(a, "--level") == 0) {
            if (++i >= argc || parse_uint(argv[i], &copts.level) != 0 ||
                copts.level < 1 || copts.level > PARC_LEVEL_MAX) {
                fprintf(stderr, "parc: -L needs an integer in %d..%d\n",
                        PARC_LEVEL_MIN, PARC_LEVEL_MAX);
                return 2;
            }
        } else if (strcmp(a, "-T") == 0 || strcmp(a, "--threads") == 0) {
            if (++i >= argc || parse_uint(argv[i], &threads) != 0 ||
                threads > PARC_THREADS_MAX) {
                fprintf(stderr, "parc: -T needs an integer in 0..%d\n",
                        PARC_THREADS_MAX);
                return 2;
            }
        } else if (strcmp(a, "-o") == 0 || strcmp(a, "--out") == 0) {
            if (++i >= argc) {
                fputs("parc: -o needs a file name\n", stderr);
                return 2;
            }
            out_path = argv[i];
        } else if (cmd == CMD_NONE &&
                   (strcmp(a, "c") == 0 || strcmp(a, "compress") == 0)) {
            cmd = CMD_C;
        } else if (cmd == CMD_NONE &&
                   (strcmp(a, "d") == 0 || strcmp(a, "decompress") == 0)) {
            cmd = CMD_D;
        } else if (cmd == CMD_NONE &&
                   (strcmp(a, "t") == 0 || strcmp(a, "verify") == 0)) {
            cmd = CMD_T;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "parc: unknown option '%s'\n", a);
            usage(stderr);
            return 2;
        } else if (in_path == NULL) {
            in_path = a;
        } else {
            fprintf(stderr, "parc: unexpected argument '%s'\n", a);
            usage(stderr);
            return 2;
        }
    }
    if (cmd == CMD_NONE) {
        usage(stderr);
        return 2;
    }
    if (cmd != CMD_C && copts.block_log != 0) {
        fputs("parc: -b only applies to compress\n", stderr);
        return 2;
    }
    if (cmd != CMD_C && copts.level != 0) {
        fputs("parc: -L only applies to compress\n", stderr);
        return 2;
    }
    if (cmd == CMD_T && out_path != NULL) {
        fputs("parc: verify writes nothing; -o makes no sense\n", stderr);
        return 2;
    }

    FILE *in = stdin;
    if (in_path && strcmp(in_path, "-") != 0) {
        in = fopen(in_path, "rb");
        if (!in) {
            fprintf(stderr, "parc: cannot open %s: %s\n", in_path,
                    strerror(errno));
            return 1;
        }
    }
    FILE *out = cmd == CMD_T ? NULL : stdout;
    if (out_path && strcmp(out_path, "-") != 0) {
        out = fopen(out_path, "wb");
        if (!out) {
            fprintf(stderr, "parc: cannot open %s: %s\n", out_path,
                    strerror(errno));
            if (in != stdin)
                fclose(in);
            return 1;
        }
    }

    if (threads == 0) { /* -T 0: one worker per online CPU */
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        threads = n < 1 ? 1
                  : n > PARC_THREADS_MAX ? PARC_THREADS_MAX
                                         : (unsigned)n;
    }

    parc_info fi;
    parc_err err;
    const char *name, *done;
    if (cmd == CMD_C) {
        name = "compress";
        done = "compressed";
        copts.threads = threads;
        err = parc_compress_stream(in, out, &copts, &fi);
    } else {
        name = cmd == CMD_D ? "decompress" : "verify";
        done = cmd == CMD_D ? "decompressed" : "verified";
        parc_dopts dopts = {threads};
        err = parc_decompress_stream(in, out, &dopts, &fi);
    }

    int rc = 0;
    if (err != PARC_OK) {
        fprintf(stderr, "parc: %s failed: %s\n", name, parc_err_str(err));
        rc = 1;
    } else if (verbose || cmd == CMD_T) {
        report(stderr, done, &fi);
    }

    if (in != stdin)
        fclose(in);
    if (out && out != stdout && fclose(out) != 0 && rc == 0) {
        fprintf(stderr, "parc: closing %s failed: %s\n", out_path,
                strerror(errno));
        rc = 1;
    }
    return rc;
}
