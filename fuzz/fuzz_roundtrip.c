/* libFuzzer target: treat the input as raw content, compress it, decompress
 * the result, and demand bit-identity. The first byte picks the block size
 * so block boundaries get fuzzed too. */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parc/parc.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1)
        return 0;
    parc_copts opts = {12 + data[0] % 6u, 0};
    data++;
    size--;

    FILE *in = size ? fmemopen((void *)data, size, "rb")
                    : fmemopen((void *)"", 1, "rb");
    if (!in)
        return 0;
    if (size == 0)
        fgetc(in); /* position the 1-byte dummy stream at EOF */

    char *comp = NULL;
    size_t comp_len = 0;
    FILE *out = open_memstream(&comp, &comp_len);
    if (!out)
        abort();
    if (parc_compress_stream(in, out, &opts, NULL) != PARC_OK)
        abort();
    fclose(in);
    fclose(out);

    FILE *cin = fmemopen(comp, comp_len, "rb");
    char *back = NULL;
    size_t back_len = 0;
    FILE *bout = open_memstream(&back, &back_len);
    if (!cin || !bout)
        abort();
    if (parc_decompress_stream(cin, bout, NULL, NULL) != PARC_OK)
        abort();
    fclose(cin);
    fclose(bout);

    if (back_len != size || (size && memcmp(back, data, size) != 0))
        abort();
    free(comp);
    free(back);
    return 0;
}
