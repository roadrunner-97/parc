#include "util/buf.h"

#include <stdlib.h>
#include <string.h>

void parc_buf_init(parc_buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void parc_buf_free(parc_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

parc_err parc_buf_reserve(parc_buf *b, size_t want)
{
    if (b->cap >= want)
        return PARC_OK;

    size_t new_cap = b->cap + b->cap / 2;
    if (new_cap < want)
        new_cap = want;

    uint8_t *new_data = (uint8_t *)realloc(b->data, new_cap);
    if (new_data == NULL)
        return PARC_ERR_NOMEM;

    b->data = new_data;
    b->cap = new_cap;
    return PARC_OK;
}

parc_err parc_buf_append(parc_buf *b, const void *src, size_t len)
{
    if (len == 0)
        return PARC_OK;

    parc_err e = parc_buf_reserve(b, b->len + len);
    if (e != PARC_OK)
        return e;

    memcpy(b->data + b->len, src, len);
    b->len += len;
    return PARC_OK;
}
