#include "parc/err.h"

/* Static, distinct, non-empty strings for every code in the enum, plus a
 * fallback for anything outside it. */

const char *parc_err_str(parc_err e)
{
    switch (e) {
    case PARC_OK:            return "ok";
    case PARC_ERR_NOMEM:     return "out of memory";
    case PARC_ERR_ARG:       return "invalid argument";
    case PARC_ERR_LIMIT:     return "capacity or size limit exceeded";
    case PARC_ERR_IO:        return "read/write failure";
    case PARC_ERR_TRUNCATED: return "input ended mid-frame";
    case PARC_ERR_CORRUPT:   return "structurally invalid frame";
    case PARC_ERR_CHECKSUM:  return "checksum mismatch";
    case PARC_ERR_VERSION:   return "unsupported format version";
    case PARC_ERR_COUNT_:    break;
    }
    return "unknown error";
}
