#ifndef PARC_ERR_H
#define PARC_ERR_H

#ifdef __cplusplus
extern "C" {
#endif

/* Error taxonomy for all of libparc. Functions that can fail return parc_err;
 * PARC_OK is guaranteed to be 0 so `if (err)` works. */
typedef enum parc_err {
    PARC_OK = 0,
    PARC_ERR_NOMEM,     /* allocation failed */
    PARC_ERR_ARG,       /* invalid argument / API misuse */
    PARC_ERR_LIMIT,     /* capacity or size limit exceeded */
    PARC_ERR_IO,        /* read/write failure */
    PARC_ERR_TRUNCATED, /* input ended mid-frame */
    PARC_ERR_CORRUPT,   /* structurally invalid frame */
    PARC_ERR_CHECKSUM,  /* checksum mismatch */
    PARC_ERR_VERSION,   /* unsupported format version */
    PARC_ERR_COUNT_     /* number of codes; not an error value */
} parc_err;

/* Returns a static, non-NULL, non-empty, human-readable description of e
 * (including PARC_OK). Every code's string is distinct. Values outside the
 * enum return "unknown error". Thread-safe, never allocates. */
const char *parc_err_str(parc_err e);

#ifdef __cplusplus
}
#endif

#endif /* PARC_ERR_H */
