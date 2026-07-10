/* libFuzzer target: arbitrary bytes into the frame decoder (verify mode).
 * Any crash, leak, hang, or sanitizer report is a bug; error returns are
 * the expected outcome. */
#define _POSIX_C_SOURCE 200809L

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "parc/parc.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    FILE *in = fmemopen((void *)data, size, "rb");
    if (!in)
        return 0;
    parc_decompress_stream(in, NULL, NULL);
    fclose(in);
    return 0;
}
