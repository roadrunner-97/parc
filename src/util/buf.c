#include "util/buf.h"

/* TODO(subagent): implement per the contract in src/util/buf.h. */

void parc_buf_init(parc_buf *b)
{
    (void)b;
}

void parc_buf_free(parc_buf *b)
{
    (void)b;
}

parc_err parc_buf_reserve(parc_buf *b, size_t want)
{
    (void)b;
    (void)want;
    return PARC_ERR_NOMEM;
}

parc_err parc_buf_append(parc_buf *b, const void *src, size_t len)
{
    (void)b;
    (void)src;
    (void)len;
    return PARC_ERR_NOMEM;
}
