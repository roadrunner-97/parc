/* Per-stage pipeline timing. The whole translation unit is gated on PARC_PROF;
 * when it is off this compiles to an empty object (the macros in prof.h expand
 * to nothing, so nothing here is ever referenced). See prof.h for the model. */
#ifdef PARC_PROF

#define _POSIX_C_SOURCE 200809L

#include "util/prof.h"

#include <string.h>
#include <time.h>

static struct {
    uint64_t ns;    /* wall-clock nanoseconds accumulated in the stage */
    uint64_t bytes; /* data volume passed through the stage */
    uint64_t calls; /* number of BEGIN/END pairs billed to the stage */
} g_stage[PARC_PROF_STAGE_COUNT];

static const char *const g_name[PARC_PROF_STAGE_COUNT] = {
    "io_read",        "io_write",       "lz_match",
    "transcode",      "entropy_encode", "entropy_decode",
    "reconstruct",    "block_hash",     "stream_hash",
};

uint64_t parc_prof_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void parc_prof_add(enum parc_prof_stage stage, uint64_t ns, uint64_t bytes)
{
    g_stage[stage].ns += ns;
    g_stage[stage].bytes += bytes;
    g_stage[stage].calls++;
}

void parc_prof_reset(void)
{
    memset(g_stage, 0, sizeof g_stage);
}

void parc_prof_report(FILE *to)
{
    uint64_t total_ns = 0;
    for (int i = 0; i < PARC_PROF_STAGE_COUNT; ++i)
        total_ns += g_stage[i].ns;
    if (total_ns == 0)
        return; /* nothing instrumented ran (e.g. the MT path) */

    fprintf(to, "\nparc profile — single-thread stage breakdown\n");
    fprintf(to, "  %-15s %10s %6s %10s %8s\n", "stage", "ms", "%", "MB/s",
            "calls");
    for (int i = 0; i < PARC_PROF_STAGE_COUNT; ++i) {
        if (g_stage[i].calls == 0)
            continue;
        double ms = (double)g_stage[i].ns / 1e6;
        double pct = 100.0 * (double)g_stage[i].ns / (double)total_ns;
        fprintf(to, "  %-15s %10.3f %5.1f%%", g_name[i], ms, pct);
        if (g_stage[i].bytes && g_stage[i].ns)
            fprintf(to, " %10.1f", (double)g_stage[i].bytes /
                                       (double)g_stage[i].ns * 1e3);
        else
            fprintf(to, " %10s", "-");
        fprintf(to, " %8llu\n", (unsigned long long)g_stage[i].calls);
    }
    fprintf(to, "  %-15s %10.3f %5.1f%%\n", "total",
            (double)total_ns / 1e6, 100.0);
}

#else
typedef int parc_prof_translation_unit_not_empty; /* avoid an empty TU */
#endif /* PARC_PROF */
